#include "w_defines.h"

#define SM_SOURCE
#define BTREE_C

#ifdef __GNUG__
#   pragma implementation "btcursor.h"
#endif

#include "sm_int_2.h"
#include "btree_p.h"
#include "btcursor.h"
#include "btree_impl.h"
#include "vec_t.h"
#include "xct.h"
#include "sm.h"

bt_cursor_t::bt_cursor_t(const lpid_t& root_pid, bool forward)
{
    w_keystr_t infimum, supremum;
    infimum.construct_neginfkey();
    supremum.construct_posinfkey();
    _init (root_pid, infimum, true, supremum,  true, forward);
}

bt_cursor_t::bt_cursor_t(
    const lpid_t&     root_pid,
    const w_keystr_t& lower, bool lower_inclusive,
    const w_keystr_t& upper, bool upper_inclusive,
    bool              forward)
{
    _init (root_pid, lower, lower_inclusive,
        upper,  upper_inclusive, forward);
}

void bt_cursor_t::_init(
    const lpid_t&     root_pid,
    const w_keystr_t& lower, bool lower_inclusive,
    const w_keystr_t& upper, bool upper_inclusive,
    bool              forward)
{
    _lower = lower;
    _upper = upper;
    
    _root_pid = root_pid;
    _lower_inclusive = lower_inclusive;
    _upper_inclusive = upper_inclusive;
    _forward = forward;
    _first_time = true;
    _dont_move_next = false;
    _eof = false;
    _pid = 0;
    _slot = -1;
    _lsn = lsn_t::null;
    _elen = 0;

    _needs_lock = g_xct_does_need_lock();
    _ex_lock = g_xct_does_ex_lock_for_select();
}


void bt_cursor_t::close()
{
    _eof = true;
    _first_time = false;
    _elen = 0;
    _slot = -1;
    _key.clear();
    _pid = 0;
    _lsn = lsn_t::null;
}

rc_t bt_cursor_t::_locate_first() {
    // at the first access, we get an intent lock on store/volume
    if (_needs_lock) {
        W_DO(ss_m::lm->intent_vol_store_lock(_root_pid.stid(), _ex_lock ? IX : IS));
    }
    
    if (_lower > _upper || (_lower == _upper && (!_lower_inclusive || !_upper_inclusive))) {
        _eof = true;
        return RCOK;
    }

    // loop because btree_impl::_ux_lock_key might return eLOCKRETRY
    while (true) {
        // find the leaf (potentially) containing the key
        const w_keystr_t &key = _forward ? _lower : _upper;
        btree_p leaf;
        bool found = false;
        W_DO( btree_impl::_ux_traverse(_root_pid, key, btree_impl::t_fence_contain, LATCH_SH, leaf));
        w_assert3 (leaf.fence_contains(key));
        _pid = leaf.pid().page;
        _lsn = leaf.lsn();

        w_assert1(leaf.is_fixed());
        w_assert1(leaf.is_leaf());

        // then find the tuple in the page
        leaf.search_leaf(key, found, _slot);

        lock_mode_t mode;
        if (found) {
            // exact match!
            _key = key;
            if (_forward) {
                if (_lower_inclusive) {
                    // let's take range lock too to reduce lock manager calls
                    mode = _ex_lock ? XX : SS;
                    _dont_move_next = true;
                } else {
                    mode = _ex_lock ? NX : NS;
                    _dont_move_next = false;
                }
            } else {
                // in backward case we definitely don't need the range part
                if (_upper_inclusive) {
                    mode = _ex_lock ? XN : SN;
                    _dont_move_next = true;
                } else {
                    // in this case, we don't need lock at all
                    mode = NL;
                    _dont_move_next = false;
                    // only in this case, _key might disappear. otherwise,
                    // _key will exist at least as a ghost entry.
                }
            }
        } else {
            // key not found. and search_leaf returns the slot the key will be inserted.
            // in other words, val(slot - 1) < key < val(slot).
            w_assert1(_slot >= 0);
            w_assert1(_slot <= leaf.nrecs());
            
            if (_forward) {
                --_slot; // subsequent next() will read the slot
                if (_slot == -1) {
                    // we are hitting the left-most of the page. (note: found=false)
                    // then, we take lock on the fence-low key
                    _dont_move_next = false;
                    leaf.copy_fence_low_key(_key);
                } else {
                    _dont_move_next = false;
                    leaf.leaf_key(_slot, _key);
                }
                mode = _ex_lock ? NX : NS;
            } else {
                // subsequent next() will read the previous slot
                --_slot;
                if (_slot == -1) {
                    // then, we need to move to even more previous slot in previous page
                    _dont_move_next = false;
                    leaf.copy_fence_low_key(_key);
                    mode = _ex_lock ? NX : NS;
                } else {
                    _dont_move_next = true;
                    leaf.leaf_key(_slot, _key);
                    // let's take range lock too to reduce lock manager calls
                    mode = _ex_lock ? XX : SS;
                }
            }
        }
        if (_needs_lock && mode != NL) {
            rc_t rc = btree_impl::_ux_lock_key (leaf, _key, LATCH_SH, mode, false);
            if (rc.is_error()) {
                if (rc.err_num() == smlevel_0::eLOCKRETRY) {
                    continue;
                } else {
                    return rc;
                }
            }
        }
        break;
    }
    return RCOK;
}

rc_t bt_cursor_t::_check_page_update(btree_p &p)
{
    // was the page changed?
    if (_pid != p.pid().page || p.lsn() != _lsn) {
        // check if the page still contains the key we are based on
        bool found = false;
        if (p.fence_contains(_key)) {
            // it still contains. just re-locate _slot
            p.search_leaf(_key, found, _slot);
        } else {
            // we have to re-locate the page
            W_DO( btree_impl::_ux_traverse(_root_pid, _key, btree_impl::t_fence_contain, LATCH_SH, p));
            p.search_leaf(_key, found, _slot);
        }
        w_assert1(found || !_needs_lock
            || (!_forward && !_upper_inclusive && !_dont_move_next)); // see _locate_first
        _pid = p.pid().page;
        _lsn = p.lsn();
    }
    return RCOK;
}

rc_t bt_cursor_t::next()
{
    if (!is_valid()) {
        return RCOK; // EOF
    }

    if (_first_time) {
        _first_time = false;
        W_DO(_locate_first ());
        if (_eof) {
            return RCOK;
        }
    }

    w_assert3(_pid);
    btree_p p;
    W_DO( p.fix(lpid_t(_root_pid.stid(), _pid), LATCH_SH) );
    w_assert3(p.is_fixed());
    w_assert3(p.pid().page == _pid);

    W_DO(_check_page_update(p));
    
    // Move one slot to the right(left if backward scan)
    bool eof_ret = false;
    W_DO(_find_next(p, eof_ret));

    if (eof_ret) {
        close();
        return RCOK;
    }

    w_assert3(p.is_fixed());
    w_assert3(p.is_leaf());

    w_assert3(_slot >= 0);
    w_assert3(_slot < p.nrecs());

    // get the current slot's values
    W_DO( _make_rec(p) );
    return RCOK;
}

rc_t bt_cursor_t::_find_next(btree_p &p, bool &eof)
{
    FUNC(bt_cursor_t::_find_next);
    while (true) {
        if (_dont_move_next) {
            _dont_move_next = false;
        } else {
            W_DO(_advance_one_slot(p, eof));
        }
        if (eof) {
            break;
        }

        // skip ghost entries
        if (p.is_ghost(_slot)) {
            continue;
        }
        break;
    }
    return RCOK;
}

rc_t bt_cursor_t::_advance_one_slot(btree_p &p, bool &eof)
{
    FUNC(bt_cursor_t::_advance_one_slot);
    w_assert1(p.is_fixed());
    w_assert1(_slot <= p.nrecs());

    if(_forward) {
        ++_slot;
    } else {
        --_slot;
    }
    eof = false;
    
    // keep following the next page.
    // because we might see empty pages to skip consecutively!
    while (true) {
        bool time2move = _forward ? (_slot >= p.nrecs()) : _slot < 0;

        if (time2move) {
            //  Move to right(left) sibling
            bool reached_end = _forward ? p.is_fence_high_supremum() : p.is_fence_low_infimum();
            if (reached_end) {
                eof = true;
                return RCOK;
            }
            // now, use fence keys to tell where the neighboring page exists
            w_keystr_t neighboring_fence;
            btree_impl::traverse_mode_t traverse_mode;
            bool only_low_fence_exact_match = false;
            if (_forward) {
                p.copy_fence_high_key(neighboring_fence);
                traverse_mode = btree_impl::t_fence_low_match;
                int d = _upper.compare(neighboring_fence);
                if (d < 0 || (d == 0 && !_upper_inclusive)) {
                    eof = true;
                    return RCOK;
                }
                if (d == 0 && _upper_inclusive) {
                    // we will check the next page, but the only
                    // possible matching is an entry with
                    // the low-fence..
                    only_low_fence_exact_match = true;
                }
            } else {
                // if we are going backwards, the current page had
                // low = [current-fence-low], high = [current-fence-high]
                // and the previous page should have 
                // low = [?], high = [current-fence-low].
                p.copy_fence_low_key(neighboring_fence);
                // let's find a page which has this value as high-fence
                traverse_mode = btree_impl::t_fence_high_match;
                int d = _lower.compare(neighboring_fence);
                if (d >= 0) {
                    eof = true;
                    return RCOK;
                }
            }
            p.unfix();
            
            // take lock for the fence key
            if (_needs_lock) {
                lockid_t lid (_root_pid.stid(), (const unsigned char*) neighboring_fence.buffer_as_keystr(), neighboring_fence.get_length_as_keystr());
                lock_mode_t lock_mode;
                if (only_low_fence_exact_match) {
                    lock_mode = _ex_lock ? XN : SN;
                } else {
                    lock_mode = _ex_lock ? XX : SS;
                }
                // we can unconditionally request lock because we already released latch
                W_DO(ss_m::lm->lock(lid, lock_mode, false));
            }
            
            // TODO this part should check if we find an exact match of fence keys.
            // because we unlatch above, it's possible to not find exact match.
            // in that case, we should change the traverse_mode to fence_contains and continue
            W_DO(btree_impl::_ux_traverse(_root_pid, neighboring_fence, traverse_mode, LATCH_SH, p));
            _slot = _forward ? 0 : p.nrecs() - 1;
            _pid = p.pid().page;
            _lsn = p.lsn();
            continue;
        }

        // take lock on the next key.
        // NOTE: until we get locks, we aren't sure the key really becomes
        // the next key. So, we use the temporary variable _tmp_next_key_buf.
        lock_mode_t mode;
        {
            p.leaf_key(_slot, _tmp_next_key_buf);
            if (_forward) {
                int d = _tmp_next_key_buf.compare(_upper);
                if (d < 0) {
                    mode = _ex_lock ? XX : SS;
                } else if (d == 0 && _upper_inclusive) {
                    mode = _ex_lock ? XN : SN;
                } else {
                    eof = true;
                    mode = NL;
                }
            } else {
                int d = _tmp_next_key_buf.compare(_lower);
                if (d > 0) {
                    mode = _ex_lock ? XX : SS;
                } else if (d == 0 && _lower_inclusive) {
                    mode = _ex_lock ? XX : SS;
                } else {
                    eof = true;
                    mode = _ex_lock ? NX : NS;
                }
            }
        }
        if (_needs_lock && mode != NL) {
            rc_t rc = btree_impl::_ux_lock_key (p, _tmp_next_key_buf, LATCH_SH, mode, false);
            if (rc.is_error()) {
                if (rc.err_num() == smlevel_0::eLOCKRETRY) {
                    W_DO(_check_page_update(p));
                    continue;
                } else {
                    return rc;
                }
            }
        }
        // okay, now we are sure the _tmp_next_key_buf is the key we want to use
        _key = _tmp_next_key_buf;
        return RCOK; // found a record! (or eof)
    }
    return RCOK;
}

rc_t bt_cursor_t::_make_rec(const btree_p& page)
{
    FUNC(bt_cursor_t::_make_rec);

    // Copy the record to buffer
    bool ghost;
    _elen = sizeof(_elbuf);
    page.dat_leaf(_slot, _elbuf, _elen, ghost);

#if W_DEBUG_LEVEL>0
    w_assert1(_elen <= sizeof(_elbuf));    
    // this should have been skipped at _advance_one_slot()
    w_assert1(!ghost);

    w_keystr_t key_again;
    page.leaf_key(_slot, key_again);
    w_assert1(key_again.compare(_key) == 0);
#endif // W_DEBUG_LEVEL>0
    
    return RCOK;
}
