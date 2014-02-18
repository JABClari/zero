/*
 * (c) Copyright 2011-2014, Hewlett-Packard Development Company, LP
 */

#ifndef LOCK_H
#define LOCK_H

#include "w_defines.h"

#include "w_okvl.h"
#include "w_okvl_inl.h"
#include "kvl_t.h"
#include "lock_s.h"

class xct_lock_info_t;
class lock_core_m;
class lil_global_table;

/**
 * \brief Lock Manager API.
 * \ingroup SSMLOCK
 * See \ref OKVL and \ref LIL.
 */
class lock_m : public smlevel_1 {
public:
    // initialize/takedown functions for thread-local state
    static void on_thread_init();
    static void on_thread_destroy();

    NORET                        lock_m(int sz);
    NORET                        ~lock_m();

    int                          collect(vtable_t&, bool names_too);

    /**
    * \brief Unsafely check that the lock table is empty for debugging
    *  and assertions at shutdown, when MT-safety shouldn't be an issue.
    */
    void                         assert_empty() const;

    /**
     * \brief Unsafely dump the lock hash table (for debugging).
     * \details Doesn't acquire the mutexes it should for safety, but
     * allows you dump the table while inside the lock manager core.
     */
    void                         dump(ostream &o);

    void                         stats(
                                    u_long & buckets_used,
                                    u_long & max_bucket_len, 
                                    u_long & min_bucket_len, 
                                    u_long & mode_bucket_len, 
                                    float & avg_bucket_len,
                                    float & var_bucket_len,
                                    float & std_bucket_len
                                    ) const;

    lil_global_table*            get_lil_global_table();

    /**
     * \brief Returns the lock granted to this transaction for the given lock ID.
     * @param[in] lock_id identifier of the lock
     * @return the lock mode this transaction has for the lock. ALL_N_GAP_N if not any.
     * \details
     * This method returns very quickly because it only checks transaction-private data.
     * @pre the current thread is the only thread running the current transaction
     */
    const okvl_mode&            get_granted_mode(const lockid_t& lock_id);

    /**
     * \brief Acquires a lock of the given mode (or stronger)
     */
    rc_t                        lock(
        const lockid_t&             n, 
        const okvl_mode&            m,
        bool                        check_only,
        timeout_in_ms               timeout = WAIT_SPECIFIED_BY_XCT);

    /**
     * Take an intent lock on the given volume.
     * lock mode must be IS/IX/S/X.
     */
    rc_t                        intent_vol_lock(vid_t vid, okvl_mode::element_lock_mode m);
    /**
     * Take an intent lock on the given store.
     */
    rc_t                        intent_store_lock(const stid_t &stid, okvl_mode::element_lock_mode m);
    /**
     * Take intent locks on the given store and its volume in the same mode.
     * This is used in usual operations like create_assoc/lookup.
     * Call intent_vol_lock() and intent_store_lock() for store-wide
     * operations where you need different lock modes for store and volume.
     * If you only need volume lock, just use intent_vol_lock().
     */
    rc_t                        intent_vol_store_lock(const stid_t &stid, okvl_mode::element_lock_mode m);
     
    // rc_t                        unlock(const lockid_t& n);

    rc_t                        unlock_duration(bool read_lock_only = false, lsn_t commit_lsn = lsn_t::null);

    void                        give_permission_to_violate(lsn_t commit_lsn = lsn_t::null);

    static void                 lock_stats(
        u_long&                      locks,
        u_long&                      acquires,
        u_long&                      cache_hits, 
        u_long&                      unlocks,
        bool                         reset);

private:
    lock_core_m*                core() const { return _core; }

    rc_t                        _lock(
        const lockid_t&              n, 
        const okvl_mode&                m,
        bool                         check_only,
        timeout_in_ms                timeout
        );

    lock_core_m*                _core;
};

#endif // LOCK_H
