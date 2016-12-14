/*
 * (c) Copyright 2011-2014, Hewlett-Packard Development Company, LP
 */

/**
 * Logging and its UNDO/REDO code for BTrees.
 * Separated from logrec.cpp.
 */

#include "btree_logrec.h"
#include "logrec_support.h"
#include "vol.h"
#include "bf_tree_cb.h"

template <class PagePtr>
void btree_insert_log::construct(
    const PagePtr page,
    const w_keystr_t&   key,
    const cvec_t&       el,
    const bool          is_sys_txn)
{
    set_size(
         (new (_data) btree_insert_t(page->root(), key, el, is_sys_txn))->size());
}

template <class PagePtr>
void btree_insert_log::undo(PagePtr page) {
    w_assert9(page == 0);
    btree_insert_t* dp = (btree_insert_t*) data();

    if (true == dp->sys_txn)
    {
        // The insertion log record was generated by a page rebalance full logging operation
        // no 'undo' in this case
        return;
    }

    w_keystr_t key;
    key.construct_from_keystr(dp->data, dp->klen);

// TODO(Restart)...
DBGOUT3( << "&&&& UNDO insertion, key: " << key);

    // ***LOGICAL*** don't grab locks during undo
    W_COERCE(btree_m::remove_as_undo(header._stid, key));
}

template <class PagePtr>
void btree_insert_log::redo(PagePtr page) {
    borrowed_btree_page_h bp(page);
    btree_insert_t* dp = (btree_insert_t*) data();

    w_assert1(bp.is_leaf());
    w_keystr_t key;
    vec_t el;
    key.construct_from_keystr(dp->data, dp->klen);
    el.put(dp->data + dp->klen, dp->elen);

// TODO(Restart)...
DBGOUT3( << "&&&& REDO insertion by replace ghost, key: " << key);

    // PHYSICAL redo
    // see btree_impl::_ux_insert()
    // at the point we called log_btree_insert,
    // we already made sure the page has a ghost
    // record for the key that is enough spacious.
    // so, we just replace the record!
    DBGOUT3( << "btree_insert_log::redo - key to replace ghost: " << key);
    w_rc_t rc = bp.replace_ghost(key, el, true /* redo */);
    if(rc.is_error()) { // can't happen. wtf?
        W_FATAL_MSG(fcINTERNAL, << "btree_insert_log::redo " );
    }
}

template <class PagePtr>
void btree_insert_nonghost_log::construct(
    const PagePtr page, const w_keystr_t &key, const cvec_t &el, const bool is_sys_txn) {
    set_size(
        (new (_data) btree_insert_t(page->root(), key, el, is_sys_txn))->size());
}

template <class PagePtr>
void btree_insert_nonghost_log::undo(PagePtr page) {
    reinterpret_cast<btree_insert_log*>(this)->undo(page); // same as btree_insert
}

template <class PagePtr>
void btree_insert_nonghost_log::redo(PagePtr page) {
    borrowed_btree_page_h bp(page);
    btree_insert_t* dp = reinterpret_cast<btree_insert_t*>(data());

    w_assert1(bp.is_leaf());
    w_keystr_t key;
    vec_t el;
    key.construct_from_keystr(dp->data, dp->klen);
    el.put(dp->data + dp->klen, dp->elen);

// TODO(Restart)...
DBGOUT3( << "&&&& REDO insertion, key: " << key);

    DBGOUT3( << "btree_insert_nonghost_log::redo - key to insert: " << key);
    bp.insert_nonghost(key, el);
}

template <class PagePtr>
void btree_update_log::construct(
    const PagePtr   page,
    const w_keystr_t&     key,
    const char* old_el, int old_elen, const cvec_t& new_el)
{
    set_size(
         (new (_data) btree_update_t(page->root(), key, old_el, old_elen, new_el))->size());
}

template <class PagePtr>
void btree_update_log::undo(PagePtr)
{
    btree_update_t* dp = (btree_update_t*) data();

    w_keystr_t key;
    key.construct_from_keystr(dp->_data, dp->_klen);
    vec_t old_el;
    old_el.put(dp->_data + dp->_klen, dp->_old_elen);

    // ***LOGICAL*** don't grab locks during undo
    rc_t rc = btree_m::update_as_undo(header._stid, key, old_el);
    if(rc.is_error()) {
        W_FATAL(rc.err_num());
    }
}

template <class PagePtr>
void btree_update_log::redo(PagePtr page)
{
    borrowed_btree_page_h bp(page);
    btree_update_t* dp = (btree_update_t*) data();

    w_assert1(bp.is_leaf());
    w_keystr_t key;
    key.construct_from_keystr(dp->_data, dp->_klen);
    vec_t old_el;
    old_el.put(dp->_data + dp->_klen, dp->_old_elen);
    vec_t new_el;
    new_el.put(dp->_data + dp->_klen + dp->_old_elen, dp->_new_elen);

    // PHYSICAL redo
    slotid_t       slot;
    bool           found;
    bp.search(key, found, slot);
    if (!found) {
        W_FATAL_MSG(fcINTERNAL, << "btree_update_log::redo(): not found");
        return;
    }
    w_rc_t rc = bp.replace_el_nolog(slot, new_el);
    if(rc.is_error()) { // can't happen. wtf?
        W_FATAL_MSG(fcINTERNAL, << "btree_update_log::redo(): couldn't replace");
    }
}

template <class PagePtr>
void btree_overwrite_log::construct(const PagePtr page, const w_keystr_t& key,
                                          const char* old_el, const char *new_el, size_t offset, size_t elen) {
    set_size(
         (new (_data) btree_overwrite_t(*page, key, old_el, new_el, offset, elen))->size());
}

template <class PagePtr>
void btree_overwrite_log::undo(PagePtr)
{
    btree_overwrite_t* dp = (btree_overwrite_t*) data();

    uint16_t elen = dp->_elen;
    uint16_t offset = dp->_offset;
    w_keystr_t key;
    key.construct_from_keystr(dp->_data, dp->_klen);
    const char* old_el = dp->_data + dp->_klen;

    // ***LOGICAL*** don't grab locks during undo
    rc_t rc = btree_m::overwrite_as_undo(header._stid, key, old_el, offset, elen);
    if(rc.is_error()) {
        W_FATAL(rc.err_num());
    }
}

template <class PagePtr>
void btree_overwrite_log::redo(PagePtr page)
{
    borrowed_btree_page_h bp(page);
    btree_overwrite_t* dp = (btree_overwrite_t*) data();

    w_assert1(bp.is_leaf());

    uint16_t elen = dp->_elen;
    uint16_t offset = dp->_offset;
    w_keystr_t key;
    key.construct_from_keystr(dp->_data, dp->_klen);
    const char* new_el = dp->_data + dp->_klen + elen;

    // PHYSICAL redo
    slotid_t       slot;
    bool           found;
    bp.search(key, found, slot);
    if (!found) {
        W_FATAL_MSG(fcINTERNAL, << "btree_overwrite_log::redo(): not found");
        return;
    }

#if W_DEBUG_LEVEL>0
    const char* old_el = dp->_data + dp->_klen;
    smsize_t cur_elen;
    bool ghost;
    const char* cur_el = bp.element(slot, cur_elen, ghost);
    w_assert1(!ghost);
    w_assert1(cur_elen >= offset + elen);
    w_assert1(::memcmp(old_el, cur_el + offset, elen) == 0);
#endif //W_DEBUG_LEVEL>0

    bp.overwrite_el_nolog(slot, offset, new_el, elen);
}

template <class PagePtr>
void btree_ghost_mark_log::construct(const PagePtr p,
                                           const vector<slotid_t>& slots,
                                           const bool is_sys_txn)
{
    set_size((new (data()) btree_ghost_t<PagePtr>(p, slots, is_sys_txn))->size());
}

template <class PagePtr>
void btree_ghost_mark_log::undo(PagePtr)
{
    // UNDO of ghost marking is to get the record back to regular state
    btree_ghost_t<PagePtr>* dp = (btree_ghost_t<PagePtr>*) data();

    if (1 == dp->sys_txn)
    {
        // The insertion log record was generated by a page rebalance full logging operation
        // no 'undo' in this case
        return;
    }

    for (size_t i = 0; i < dp->cnt; ++i) {
        w_keystr_t key (dp->get_key(i));

// TODO(Restart)...
DBGOUT3( << "&&&& UNDO deletion by remove ghost mark, key: " << key);

        rc_t rc = btree_m::undo_ghost_mark(header._stid, key);
        if(rc.is_error()) {
            cerr << " key=" << key << endl << " rc =" << rc << endl;
            W_FATAL(rc.err_num());
        }
    }
}

template <class PagePtr>
void btree_ghost_mark_log::redo(PagePtr page)
{
    // REDO is physical. mark the record as ghost again.
    w_assert1(page);
    borrowed_btree_page_h bp(page);

    w_assert1(bp.is_leaf());
    btree_ghost_t<PagePtr>* dp = (btree_ghost_t<PagePtr>*) data();

    for (size_t i = 0; i < dp->cnt; ++i) {
        w_keystr_t key (dp->get_key(i));

            // If full logging, data movement log records are generated to remove records
            // from source, we set the new fence keys for source page in page_rebalance
            // log record which happens before the data movement log records.
            // Which means the source page might contain records which will be moved
            // out after the page_rebalance log records.  Do not validate the fence keys
            // if full logging

            // Assert only if minmal logging
            w_assert2(bp.fence_contains(key));

        bool found;
        slotid_t slot;

        bp.search(key, found, slot);
        // If doing page driven REDO, page_rebalance initialized the
        // target page (foster child).
        if (!found) {
            cerr << " key=" << key << endl << " not found in btree_ghost_mark_log::redo" << endl;
            w_assert1(false); // something unexpected, but can go on.
        }
        else
        {
            // TODO(Restart)...
            DBGOUT3( << "&&&& REDO deletion, not part of full logging, key: " << key);

            bp.mark_ghost(slot);
        }
    }
}

template <class PagePtr>
void btree_ghost_reclaim_log::construct(const PagePtr p,
                                                 const vector<slotid_t>& slots)
{
    // ghost reclaim is single-log system transaction. so, use data_ssx()
    set_size((new (data_ssx()) btree_ghost_t<PagePtr>(p, slots, false))->size());
    w_assert0(is_single_sys_xct());
}

template <class PagePtr>
void btree_ghost_reclaim_log::redo(PagePtr page)
{
    // REDO is to defrag it again
    borrowed_btree_page_h bp(page);
    // TODO actually should reclaim only logged entries because
    // locked entries might have been avoided.
    // (but in that case shouldn't defragging the page itself be avoided?)
    rc_t rc = btree_impl::_sx_defrag_page(bp);
    if (rc.is_error()) {
        W_FATAL(rc.err_num());
    }
}

template <class PagePtr>
void btree_ghost_reserve_log::construct (
    const PagePtr /*p*/, const w_keystr_t& key, int element_length) {
    // ghost creation is single-log system transaction. so, use data_ssx()
    set_size((new (data_ssx()) btree_ghost_reserve_t(key, element_length))->size());
    w_assert0(is_single_sys_xct());
}

template <class PagePtr>
void btree_ghost_reserve_log::redo(PagePtr page) {
    // REDO is to physically make the ghost record
    borrowed_btree_page_h bp(page);
    // ghost creation is single-log system transaction. so, use data_ssx()
    btree_ghost_reserve_t* dp = (btree_ghost_reserve_t*) data_ssx();

    // PHYSICAL redo.
    w_assert1(bp.is_leaf());
    bp.reserve_ghost(dp->data, dp->klen, dp->element_length);
    w_assert3(bp.is_consistent(true, true));
}

template <class PagePtr>
void btree_norec_alloc_log::construct(const PagePtr p, const PagePtr,
    PageID new_page_id, const w_keystr_t& fence, const w_keystr_t& chain_fence_high) {
    set_size((new (data_ssx()) btree_norec_alloc_t<PagePtr>(p,
        new_page_id, fence, chain_fence_high))->size());
}

template <class PagePtr>
void btree_norec_alloc_log::redo(PagePtr p) {
    w_assert1(is_single_sys_xct());
    borrowed_btree_page_h bp(p);
    btree_norec_alloc_t<PagePtr> *dp =
        reinterpret_cast<btree_norec_alloc_t<PagePtr>*>(data_ssx());

    const lsn_t &new_lsn = lsn_ck();
    w_keystr_t fence, chain_high;
    fence.construct_from_keystr(dp->_data, dp->_fence_len);
    chain_high.construct_from_keystr(dp->_data + dp->_fence_len, dp->_chain_high_len);

    PageID target_pid = p->pid();
    DBGOUT3 (<< *this << ": new_lsn=" << new_lsn
        << ", target_pid=" << target_pid << ", bp.lsn=" << bp.get_page_lsn());
    if (target_pid == dp->_page2_pid) {
        // we are recovering "page2", which is foster-child.
        w_assert0(target_pid == dp->_page2_pid);
        // This log is also a page-allocation log, so redo the page allocation.
        W_COERCE(smlevel_0::vol->alloc_a_page(dp->_page2_pid, true /* redo */));
        PageID pid = dp->_page2_pid;
        // initialize as an empty child:
        bp.format_steal(new_lsn, pid, header._stid,
                        dp->_root_pid, dp->_btree_level, 0, lsn_t::null,
                        dp->_foster_pid, dp->_foster_emlsn, fence, fence, chain_high, false);
    } else {
        // we are recovering "page", which is foster-parent.
        bp.accept_empty_child(new_lsn, dp->_page2_pid, true /*from redo*/);
    }
}

template <class PagePtr>
void btree_foster_adopt_log::construct(const PagePtr /*p*/, const PagePtr p2,
    PageID new_child_pid, lsn_t new_child_emlsn, const w_keystr_t& new_child_key) {
    set_size((new (data_ssx()) btree_foster_adopt_t(
        p2->pid(), new_child_pid, new_child_emlsn, new_child_key))->size());
}

template <class PagePtr>
void btree_foster_adopt_log::redo(PagePtr p) {
    w_assert1(is_single_sys_xct());
    borrowed_btree_page_h bp(p);
    btree_foster_adopt_t *dp = reinterpret_cast<btree_foster_adopt_t*>(data_ssx());

    w_keystr_t new_child_key;
    new_child_key.construct_from_keystr(dp->_data, dp->_new_child_key_len);

    PageID target_pid = p->pid();
    DBGOUT3 (<< *this << " target_pid=" << target_pid << ", new_child_pid="
        << dp->_new_child_pid << ", new_child_key=" << new_child_key);
    if (target_pid == dp->_page2_pid) {
        // we are recovering "page2", which is real-child.
        w_assert0(target_pid == dp->_page2_pid);
        btree_impl::_ux_adopt_foster_apply_child(bp);
    } else {
        // we are recovering "page", which is real-parent.
        btree_impl::_ux_adopt_foster_apply_parent(bp, dp->_new_child_pid,
                                                  dp->_new_child_emlsn, new_child_key);
    }
}

template <class PagePtr>
void btree_split_log::construct(
        const PagePtr child_p,
        const PagePtr parent_p,
        uint16_t move_count,
        const w_keystr_t& new_high_fence,
        const w_keystr_t& new_chain
)
{
    btree_bulk_delete_t* bulk =
        new (data_ssx()) btree_bulk_delete_t(parent_p->pid(),
                    child_p->pid(), move_count,
                    new_high_fence, new_chain);
    page_img_format_t<PagePtr>* format = new (data_ssx() + bulk->size())
        page_img_format_t<PagePtr>(child_p);

    // Logrec will have the child pid as main pid (i.e., destination page).
    // Parent pid is stored in btree_bulk_delete_t, which is a
    // multi_page_log_t (i.e., source page)
    set_size(bulk->size() + format->size());
}

template <class PagePtr>
void btree_split_log::redo(PagePtr p)
{
    btree_bulk_delete_t* bulk = (btree_bulk_delete_t*) data_ssx();
    page_img_format_t<PagePtr>* format = (page_img_format_t<PagePtr>*)
        (data_ssx() + bulk->size());

    if (p->pid() == bulk->new_foster_child) {
        // redoing the foster child
        format->apply(p);
    }
    else {
        // redoing the foster parent
        borrowed_btree_page_h bp(p);
        w_assert1(bp.nrecs() > bulk->move_count);
        bp.delete_range(bp.nrecs() - bulk->move_count, bp.nrecs());

        w_keystr_t new_high_fence, new_chain;
        bulk->get_keys(new_high_fence, new_chain);

        bp.set_foster_child(bulk->new_foster_child, new_high_fence, new_chain);
    }
}

template <class PagePtr>
void btree_compress_page_log::construct(
        const PagePtr /*page*/,
        const w_keystr_t& low,
        const w_keystr_t& high,
        const w_keystr_t& chain)
{
    uint16_t low_len = low.get_length_as_keystr();
    uint16_t high_len = high.get_length_as_keystr();
    uint16_t chain_len = chain.get_length_as_keystr();

    char* ptr = data_ssx();
    memcpy(ptr, &low_len, sizeof(uint16_t));
    ptr += sizeof(uint16_t);
    memcpy(ptr, &high_len, sizeof(uint16_t));
    ptr += sizeof(uint16_t);
    memcpy(ptr, &chain_len, sizeof(uint16_t));
    ptr += sizeof(uint16_t);

    low.serialize_as_keystr(ptr);
    ptr += low_len;
    high.serialize_as_keystr(ptr);
    ptr += high_len;
    chain.serialize_as_keystr(ptr);
    ptr += chain_len;

    set_size(ptr - data_ssx());
}

template <class PagePtr>
void btree_compress_page_log::redo(PagePtr p)
{
    char* ptr = data_ssx();

    uint16_t low_len = *((uint16_t*) ptr);
    ptr += sizeof(uint16_t);
    uint16_t high_len = *((uint16_t*) ptr);
    ptr += sizeof(uint16_t);
    uint16_t chain_len = *((uint16_t*) ptr);
    ptr += sizeof(uint16_t);

    w_keystr_t low, high, chain;
    low.construct_from_keystr(ptr, low_len);
    ptr += low_len;
    high.construct_from_keystr(ptr, high_len);
    ptr += high_len;
    chain.construct_from_keystr(ptr, chain_len);

    borrowed_btree_page_h bp(p);
    bp.compress(low, high, chain, true /* redo */);
}

template void btree_norec_alloc_log::template construct<btree_page_h*>(
        btree_page_h*, btree_page_h*, PageID, const w_keystr_t&, const w_keystr_t&);

template void btree_split_log::template construct<btree_page_h*>(
        btree_page_h* child_p,
        btree_page_h* parent_p,
        uint16_t move_count,
        const w_keystr_t& new_high_fence,
        const w_keystr_t& new_chain
);

template void btree_foster_adopt_log::template construct<btree_page_h*>(
        btree_page_h* p, btree_page_h* p2,
    PageID new_child_pid, lsn_t new_child_emlsn, const w_keystr_t& new_child_key);

template void btree_insert_nonghost_log::template construct<btree_page_h*>(
    btree_page_h* page, const w_keystr_t &key, const cvec_t &el, const bool is_sys_txn);

template struct btree_ghost_t<btree_page_h*>;

template void btree_ghost_mark_log::template construct<btree_page_h*>(
        btree_page_h*, const vector<slotid_t>& slots, const bool is_sys_txn);

template void btree_insert_log::template construct<btree_page_h*>(
    btree_page_h* page,
    const w_keystr_t&   key,
    const cvec_t&       el,
    const bool          is_sys_txn);

template void btree_ghost_reclaim_log::template construct<btree_page_h*>(
        btree_page_h* p, const vector<slotid_t>& slots);

template void btree_compress_page_log::template construct<btree_page_h*>(
        btree_page_h* page,
        const w_keystr_t& low,
        const w_keystr_t& high,
        const w_keystr_t& chain);

template void btree_ghost_reserve_log::template construct<btree_page_h*>
    (btree_page_h* p, const w_keystr_t& key, int element_length);

template void btree_overwrite_log::template construct<btree_page_h*>
    (btree_page_h* page, const w_keystr_t& key,
    const char* old_el, const char *new_el, size_t offset, size_t elen) ;

template void btree_update_log::template construct<btree_page_h*>(
    btree_page_h*   page,
    const w_keystr_t&     key,
    const char* old_el, int old_elen, const cvec_t& new_el);

template void btree_insert_log::template undo<fixable_page_h*>(fixable_page_h*);
template void btree_insert_nonghost_log::template undo<fixable_page_h*>(fixable_page_h*);
template void btree_update_log::template undo<fixable_page_h*>(fixable_page_h*);
template void btree_overwrite_log::template undo<fixable_page_h*>(fixable_page_h*);
template void btree_ghost_mark_log::template undo<fixable_page_h*>(fixable_page_h*);

template void btree_norec_alloc_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_insert_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_insert_nonghost_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_update_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_overwrite_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_ghost_mark_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_ghost_reclaim_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_ghost_reserve_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_foster_adopt_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_split_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_compress_page_log::template redo<btree_page_h*>(btree_page_h*);

template void btree_norec_alloc_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_insert_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_insert_nonghost_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_update_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_overwrite_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_ghost_mark_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_ghost_reclaim_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_ghost_reserve_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_foster_adopt_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_split_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_compress_page_log::template redo<fixable_page_h*>(fixable_page_h*);
