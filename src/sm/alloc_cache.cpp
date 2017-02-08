/*
 * (c) Copyright 2011-2013, Hewlett-Packard Development Company, LP
 */

#include "w_defines.h"

#include "sm_base.h"
#include "vol.h"
#include "alloc_cache.h"
#include "smthread.h"
#include "xct_logger.h"

const size_t alloc_cache_t::extent_size = alloc_page::bits_held;

alloc_cache_t::alloc_cache_t(stnode_cache_t& stcache, bool virgin)
    : stcache(stcache)
{
    vector<StoreID> stores;
    stcache.get_used_stores(stores);

    if (virgin) {
        // Extend 0 and stnode pid are always allocated
        loaded_extents.push_back(true);
        // first extent (which has stnode page) is assigned to store 0,
        // which baiscally means the extent does not belong to any particular
        // store
        last_alloc_page.push_back(stnode_page::stpid);
    }
    else {
        // Load last extent eagerly and the rest of them on demand
        for (auto s : stores) {
            last_alloc_page.resize(s, 0);
            extent_id_t ext = stcache.get_last_extent(s);
            loaded_extents.resize(ext + 1, false);
            W_COERCE(load_alloc_page(ext, true));
        }
    }
}

rc_t alloc_cache_t::load_alloc_page(extent_id_t ext, bool is_last_ext)
{
    spinlock_write_critical_section cs(&_latch);

    // protect against race on concurrent loads
    if (loaded_extents[ext]) {
        return RCOK;
    }

    PageID alloc_pid = ext * extent_size;
    fixable_page_h p;
    W_DO(p.fix_direct(alloc_pid, LATCH_EX, false, false));
    alloc_page* page = (alloc_page*) p.get_generic_page();

    if (is_last_ext) {
        // we know that at least all pids in lower extents were once allocated
        last_alloc_page[page->store_id] = alloc_pid;
    }

    size_t last_alloc = 0;
    size_t j = alloc_page::bits_held;
    while (j > 0) {
        if (page->get_bit(j)) {
            if (last_alloc == 0) {
                last_alloc = j;
                if (is_last_ext) {
                    last_alloc_page[page->store_id] = alloc_pid + j;
                }
            }
        }
        else if (last_alloc != 0) {
            freed_pages.insert(alloc_pid + j);
        }

        j--;
    }

    page_lsns[p.pid()] = p.lsn();
    loaded_extents[ext] = true;

    // pass argument evict=true because we won't be maintaining the page
    p.unfix(true);

    return RCOK;
}

PageID alloc_cache_t::get_last_allocated_pid(StoreID s) const
{
    spinlock_read_critical_section cs(&_latch);
    return last_alloc_page[s];
}

PageID alloc_cache_t::get_last_allocated_pid() const
{
    spinlock_read_critical_section cs(&_latch);
    return _get_last_allocated_pid_internal();
}

PageID alloc_cache_t::_get_last_allocated_pid_internal() const
{
    PageID max = 0;
    for (auto p : last_alloc_page) {
        if (p > max) { max = p; }
    }
    return max;
}

lsn_t alloc_cache_t::get_page_lsn(PageID pid)
{
    spinlock_read_critical_section cs(&_latch);
    map<PageID, lsn_t>::const_iterator it = page_lsns.find(pid);
    if (it == page_lsns.end()) { return lsn_t::null; }
    return it->second;
}

bool alloc_cache_t::is_allocated(PageID pid)
{
    // No latching required to check if loaded. Any races will be
    // resolved inside load_alloc_page
    extent_id_t ext = pid / extent_size;
    if (!loaded_extents[ext]) {
        W_COERCE(load_alloc_page(ext, false));
    }

    auto max_pid = get_last_allocated_pid();

    spinlock_read_critical_section cs(&_latch);

    // loaded cannot go from true to false, so this is safe
    w_assert0(loaded_extents[ext]);

    if (pid > max_pid) { return false; }

    pid_set::const_iterator iter = freed_pages.find(pid);
    return (iter == freed_pages.end());
}

rc_t alloc_cache_t::sx_allocate_page(PageID& pid, StoreID stid, bool redo)
{
    spinlock_write_critical_section cs(&_latch);

    if (redo) {
        // all space before this pid must not be contiguous free space
        if (last_alloc_page[stid] < pid) {
            last_alloc_page[stid] = pid;
        }
        // if pid is on freed list, remove
        pid_set::iterator iter = freed_pages.find(pid);
        if (iter != freed_pages.end()) {
            freed_pages.erase(iter);
        }
    }
    else {
        if (last_alloc_page.size() <= stid) {
            last_alloc_page.resize(stid + 1, 0);
        }

        pid = last_alloc_page[stid] + 1;
        w_assert1(stid != 0 || pid != stnode_page::stpid);

        if (pid == 1 || pid % extent_size == 0) {
            extent_id_t ext = _get_last_allocated_pid_internal() / extent_size + 1;
            pid = ext * extent_size + 1;
            W_DO(stcache.sx_append_extent(stid, ext));
        }

        last_alloc_page[stid] = pid;

        // CS TODO: page allocation should transfer ownership instead of just
        // marking the page as allocated; otherwise, zombie pages may appear
        // due to system failures after allocation but before setting the
        // pointer on the new owner/parent page. To fix this, an SSX to
        // allocate an emptry b-tree child would be the best option.

        // Entry in page_lsns array is updated by the log insertion
        extent_id_t ext = pid / extent_size;
        Logger::log_page_chain<alloc_page_log>(page_lsns[ext * extent_size], pid);
    }

    return RCOK;
}

rc_t alloc_cache_t::sx_deallocate_page(PageID pid, bool redo)
{
    spinlock_write_critical_section cs(&_latch);

    // Just add to list of freed pages
    freed_pages.insert(pid);

    if (!redo) {
        // Entry in page_lsns array is updated by the log insertion
        extent_id_t ext = pid / extent_size;
        Logger::log_page_chain<dealloc_page_log>(page_lsns[ext * extent_size], pid);
    }

    return RCOK;
}

rc_t alloc_cache_t::write_dirty_pages(lsn_t rec_lsn)
{
    generic_page* buf = NULL;
    lsn_t page_lsn = lsn_t::null;
    extent_id_t last_extent = 0;

    // We just have to iterate over the extents in the page_lsns table, since
    // those are the only ones which were modified since the system started.
    {
        spinlock_read_critical_section cs (&_latch);
        last_extent = get_last_allocated_pid() / extent_size;
    }

    map<PageID, lsn_t>::const_iterator iter;
    for (extent_id_t ext = 0; ext <= last_extent; ext++) {
        PageID alloc_pid = ext * extent_size;
        // While in the critical section, just verify if the extent alloc page
        // needs to be written, to avoid blocking threads trying to allocate
        // pages for too long.
        {
            spinlock_read_critical_section cs(&_latch);
            iter = page_lsns.find(alloc_pid);
            if (iter == page_lsns.end()) { continue; }
            if (iter->second > rec_lsn) { continue; }
            page_lsn = iter->second;
        }

        if (!buf) {
            int res = posix_memalign((void**) &buf, SM_PAGESIZE, SM_PAGESIZE);
            w_assert0(res == 0);
        }

        // Read old page image into buffer, replay updates with SPR, and write
        // it back
        W_DO(smlevel_0::vol->read_page_verify(alloc_pid, buf, page_lsn));
        W_DO(smlevel_0::vol->write_page(alloc_pid, buf));
        Logger::log_sys<page_write_log>(alloc_pid, rec_lsn, 1);
    }

    if (buf) { delete[] buf; }

    return RCOK;
}
