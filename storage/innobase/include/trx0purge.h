/*****************************************************************************

Copyright (c) 1996, 2019, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/trx0purge.h
 Purge old versions

 Created 3/26/1996 Heikki Tuuri
 *******************************************************/

#ifndef trx0purge_h
#define trx0purge_h

#include "fil0fil.h"
#include "mtr0mtr.h"
#include "page0page.h"
#include "que0types.h"
#include "read0types.h"
#include "trx0sys.h"
#include "trx0types.h"
#include "univ.i"
#include "usr0sess.h"
#ifdef UNIV_HOTBACKUP
#include "trx0sys.h"
#endif /* UNIV_HOTBACKUP */

#include "lizard0purge.h"
#include "lizard0read0types.h"
#include "lizard0scn.h"
#include "lizard0undo0types.h"

/** The global data structure coordinating a purge */
extern trx_purge_t *purge_sys;

/** Calculates the file address of an undo log header when we have the file
 address of its history list node.
 @return file address of the log */
UNIV_INLINE
fil_addr_t trx_purge_get_log_from_hist(
    fil_addr_t node_addr); /*!< in: file address of the history
                           list node of the log */

/** Creates the global purge system control structure and inits the history
mutex.
@param[in]      n_purge_threads   number of purge threads
@param[in,out]  purge_queue       UNDO log min binary heap */
void trx_purge_sys_create(ulint n_purge_threads,
                          lizard::purge_heap_t *purge_heap);

/** Frees the global purge system control structure. */
void trx_purge_sys_close(void);

/** Get current purged GCN number */
gcn_t gcs_get_purged_gcn();

/************************************************************************
Adds the update undo log as the first log in the history list. Removes the
update undo log segment from the rseg slot if it is too big for reuse. */
void trx_purge_add_update_undo_to_history(
    trx_t *trx,               /*!< in: transaction */
    trx_undo_ptr_t *undo_ptr, /*!< in: update undo log. */
    page_t *undo_page,        /*!< in: update undo log header page,
                              x-latched */
    bool update_rseg_history_len,
    /*!< in: if true: update rseg history
    len else skip updating it. */
    ulint n_added_logs, /*!< in: number of logs added */
    mtr_t *mtr);        /*!< in: mtr */

/** This function runs a purge batch.
 @return number of undo log pages handled in the batch */
ulint trx_purge(ulint n_purge_threads, /*!< in: number of purge tasks to
                                       submit to task queue. */
                ulint limit,           /*!< in: the maximum number of
                                       records to purge in one batch */
                bool truncate,         /*!< in: truncate history if true */
                bool *blocked = NULL); /*!< out: is blocked by retention */

/** Stop purge and wait for it to stop, move to PURGE_STATE_STOP. */
void trx_purge_stop(void);
/** Resume purge, move to PURGE_STATE_RUN. */
void trx_purge_run(void);

/** Purge states */
enum purge_state_t {
  PURGE_STATE_INIT,    /*!< Purge instance created */
  PURGE_STATE_RUN,     /*!< Purge should be running */
  PURGE_STATE_STOP,    /*!< Purge should be stopped */
  PURGE_STATE_EXIT,    /*!< Purge has been shutdown */
  PURGE_STATE_DISABLED /*!< Purge was never started */
};

/** Get the purge state.
 @return purge state. */
purge_state_t trx_purge_state(void);

namespace lizard {
struct TxnUndoRsegsIterator;
}  // namespace lizard

/** This is the purge pointer/iterator. We need both the undo no and the
transaction no up to which purge has parsed and applied the records. */
struct purge_iter_t {
  purge_iter_t() : scn(), undo_no(), undo_rseg_space(SPACE_UNKNOWN) {
    // Do nothing
  }

  scn_t scn; /*!< Purge has advanced past all
             transactions whose SCN number is less or equal
             than this */

  undo_no_t undo_no; /*!< Purge has advanced past all records
                     whose undo number is less than this */
  space_id_t undo_rseg_space;
  /*!< Last undo record resided in this
  space id. */
  trx_id_t modifier_trx_id;
  /*!< the transaction that created the
  undo log record. Modifier trx id.*/
};

/* Namespace to hold all the related functions and variables needed
to truncate an undo tablespace. */
namespace undo {

/** Magic Number to indicate truncate action is complete. */
const ib_uint32_t s_magic = 76845412;

/** Truncate Log file Prefix. */
const char *const s_log_prefix = "undo_";

/** Truncate Log file Extension. */
const char *const s_log_ext = "trunc.log";

/** The currently used undo space IDs for an undo space number
along with a boolean showing whether the undo space number is in use. */
struct space_id_account {
  space_id_t space_id;
  bool in_use;
};

/** List of currently used undo space IDs for each undo space number
along with a boolean showing whether the undo space number is in use. */
extern struct space_id_account *space_id_bank;

/** Check if the space_id is an undo space ID in the reserved range.
@param[in]	space_id	undo tablespace ID
@return true if it is in the reserved undo space ID range. */
inline bool is_reserved(space_id_t space_id) {
  return (space_id >= dict_sys_t::s_min_undo_space_id &&
          space_id <= dict_sys_t::s_max_undo_space_id);
}

/** Convert an undo space number (from 1 to 127) into the undo space_id,
given an index indicating which space_id from the pool assigned to that
undo number.
@param[in]  space_num  undo tablespace number
@param[in]  ndx        index of the space_id within that undo number
@return space_id of the undo tablespace */
inline space_id_t num2id(space_id_t space_num, size_t ndx) {
  ut_ad(space_num > 0);
  ut_ad(space_num <= FSP_MAX_UNDO_TABLESPACES);
  ut_ad(ndx < dict_sys_t::undo_space_id_range);

  space_id_t space_id = dict_sys_t::s_max_undo_space_id + 1 - space_num -
                        static_cast<space_id_t>(ndx * FSP_MAX_UNDO_TABLESPACES);

  return (space_id);
}

/** Convert an undo space number (from 1 to 127) into an undo space_id.
Use the undo::space_id_bank to return the curent space_id assigned to
that undo number.
@param[in]  space_num   undo tablespace number
@return space_id of the undo tablespace */
inline space_id_t num2id(space_id_t space_num) {
  ut_ad(space_num > 0);
  ut_ad(space_num <= FSP_MAX_UNDO_TABLESPACES);

  size_t slot = space_num - 1;

  /* The space_id_back is normally protected by undo::spaces::m_latch.
  But this can only be called on a specific slot when truncation is not
  happening on that slot, i.e. the undo tablespace is in use. */
  ut_ad(undo::space_id_bank[slot].in_use);

  return (undo::space_id_bank[slot].space_id);
}

/* clang-format off */
/** Convert an undo space ID into an undo space number.
NOTE: This may be an undo space_id from a pre-exisiting 5.7
database which used space_ids from 1 to 127.  If so, the
space_id is the space_num.
The space_ids are assigned to number ranges in reverse from high to low.
In addition, the first space IDs for each undo number occur sequentionally
and descending before the second space_id.

Since s_max_undo_space_id = 0xFFFFFFEF, FSP_MAX_UNDO_TABLESPACES = 127
and undo_space_id_range = 512:
  Space ID   Space Num    Space ID   Space Num   ...  Space ID   Space Num
  0xFFFFFFEF      1       0xFFFFFFEe       2     ...  0xFFFFFF71    127
  0xFFFFFF70      1       0xFFFFFF6F       2     ...  0xFFFFFEF2    127
  0xFFFFFEF1      1       0xFFFFFEF0       2     ...  0xFFFFFE73    127
...

This is done to maintain backward compatibility to when there was only one
space_id per undo space number.
@param[in]	space_id	undo tablespace ID
@return space number of the undo tablespace */
/* clang-format on */
inline space_id_t id2num(space_id_t space_id) {
  if (!is_reserved(space_id)) {
    return (space_id);
  }

  return (((dict_sys_t::s_max_undo_space_id - space_id) %
           FSP_MAX_UNDO_TABLESPACES) +
          1);
}

/* Given a reserved undo space_id, return the next space_id for the associated
undo space number. */
inline space_id_t id2next_id(space_id_t space_id) {
  ut_ad(is_reserved(space_id));

  space_id_t space_num = id2num(space_id);
  space_id_t first_id = dict_sys_t::s_max_undo_space_id + 1 - space_num;
  space_id_t last_id = first_id - (FSP_MAX_UNDO_TABLESPACES *
                                   (dict_sys_t::undo_space_id_range - 1));

  return (space_id == SPACE_UNKNOWN || space_id == last_id
              ? first_id
              : space_id - FSP_MAX_UNDO_TABLESPACES);
}

/** Initialize the undo tablespace space_id bank which is a lock free
repository for information about the space IDs used for undo tablespaces.
It is used during creation in order to assign an unused space number and
during truncation in order to assign the next space_id within that
space_number range. */
void init_space_id_bank();

/** Note that the undo space number for a space ID is being used.
Put that space_id into the space_id_bank.
@param[in] space_id  undo tablespace number */
void use_space_id(space_id_t space_id);

/** Mark that the given undo space number is being used and
return the next available space_id for that space number.
@param[in]  space_num  undo tablespace number
@return the next tablespace ID to use */
space_id_t use_next_space_id(space_id_t space_num);

/** Mark an undo number associated with a given space_id as unused and
available to be resused.  This happens when the fil_space_t is closed
associated with a drop undo tablespace.
@param[in] space_id  Undo Tablespace ID */
void unuse_space_id(space_id_t space_id);

/** Given a valid undo space_id or SPACE_UNKNOWN, return the next space_id
for the given space number.
@param[in]  space_id   undo tablespace ID
@param[in]  space_num  undo tablespace number
@return the next tablespace ID to use */
space_id_t next_space_id(space_id_t space_id, space_id_t space_num);

/** Given a valid undo space_id, return the next space_id for that
space number.
@param[in]  space_id  undo tablespace ID
@return the next tablespace ID to use */
space_id_t next_space_id(space_id_t space_id);

/** Return the next available undo space ID to be used for a new explicit
undo tablespaces.
@retval if success, next available undo space number.
@retval if failure, SPACE_UNKNOWN */
space_id_t get_next_available_space_num();

/** Build a standard undo tablespace name from a space_id.
@param[in]	space_id	id of the undo tablespace.
@return tablespace name of the undo tablespace file */
char *make_space_name(space_id_t space_id);

/** Build a standard undo tablespace file name from a space_id.
@param[in]	space_id	id of the undo tablespace.
@return file_name of the undo tablespace file */
char *make_file_name(space_id_t space_id);

/** An undo::Tablespace object is used to easily convert between
undo_space_id and undo_space_num and to create the automatic file_name
and space name.  In addition, it is used in undo::Tablespaces to track
the trx_rseg_t objects in an Rsegs vector. So we do not allocate the
Rsegs vector for each object, only when requested by the constructor. */
struct Tablespace {
  /** Constructor
  @param[in]  id    tablespace id */
  explicit Tablespace(space_id_t id)
      : m_id(id),
        m_num(undo::id2num(id)),
        m_implicit(true),
        m_new(false),
        m_space_name(),
        m_file_name(),
        m_log_file_name(),
        m_rsegs(),
        m_txn(false) {}

  /** Copy Constructor
  @param[in]  other    undo tablespace to copy */
  Tablespace(Tablespace &other)
      : m_id(other.id()),
        m_num(undo::id2num(other.id())),
        m_implicit(other.is_implicit()),
        m_new(other.is_new()),
        m_space_name(),
        m_file_name(),
        m_log_file_name(),
        m_rsegs(),
        m_txn(other.is_txn()) {
    ut_ad(m_id == 0 || is_reserved(m_id));

    set_space_name(other.space_name());
    set_file_name(other.file_name());

    /* When the copy constructor is used, add an Rsegs
    vector. This constructor is only used in the global
    undo::Tablespaces object where rollback segments are
    tracked. */
    m_rsegs = UT_NEW_NOKEY(Rsegs());
  }

  /** Destructor */
  ~Tablespace() {
    if (m_space_name != nullptr) {
      ut_free(m_space_name);
      m_space_name = nullptr;
    }

    if (m_file_name != nullptr) {
      ut_free(m_file_name);
      m_file_name = nullptr;
    }

    if (m_log_file_name != nullptr) {
      ut_free(m_log_file_name);
      m_log_file_name = nullptr;
    }

    /* Clear the cached rollback segments.  */
    if (m_rsegs != nullptr) {
      UT_DELETE(m_rsegs);
      m_rsegs = nullptr;
    }
  }

  /* Determine if this undo space needs to be truncated.
  @return true if it should be truncated, false if not. */
  bool needs_truncation() {
    /* If it is already inactive, even implicitly, then proceed. */
    if (m_rsegs->is_inactive_implicit() || m_rsegs->is_inactive_explicit()) {
      return (true);
    }

    /* If implicit undo truncation is turned off, or if the rsegs don't exist
    yet, don't bother checking the size. */
    if (!srv_undo_log_truncate || m_rsegs == nullptr || m_rsegs->is_empty() ||
        m_rsegs->is_init()) {
      return (false);
    }

    ut_ad(m_rsegs->is_active());

    page_no_t trunc_size = ut_max(
        static_cast<page_no_t>(srv_max_undo_tablespace_size / srv_page_size),
        static_cast<page_no_t>(SRV_UNDO_TABLESPACE_SIZE_IN_PAGES));
    if (fil_space_get_size(id()) > trunc_size) {
      return (true);
    }

    return (false);
  }

  /** Change the space_id from its current value.
  @param[in]  space_id  The new undo tablespace ID */
  void set_space_id(space_id_t space_id);

  /** Replace the standard undo space name if it exists with a copy
  of the undo tablespace name provided.
  @param[in]  new_space_name  non-standard undo space name */
  void set_space_name(const char *new_space_name);

  /** Get the undo tablespace name. Make it if not yet made.
  NOTE: This is only called from stack objects so there is no
  race condition. If it is ever called from a shared object
  like undo::spaces, then it must be protected by the caller.
  @return tablespace name created from the space_id */
  char *space_name() {
    if (m_space_name == nullptr) {
#ifndef UNIV_HOTBACKUP
      m_space_name = make_space_name(m_id);
#endif /* !UNIV_HOTBACKUP */
    }

    return (m_space_name);
  }

  /** Replace the standard undo file name if it exists with a copy
  of the file name provided. This name can come in three forms:
  absolute path, relative path, and basename.  Undo ADD DATAFILE
  does not accept a relative path.  So if that comes in here, it
  was the scaneed name and is relative to the datadir.
  If this is just a basename, add it to srv_undo_dir.
  @param[in]  file_name  explicit undo file name */
  void set_file_name(const char *file_name);

  /** Get the undo space filename. Make it if not yet made.
  NOTE: This is only called from stack objects so there is no
  race condition. If it is ever called from a shared object
  like undo::spaces, then it must be protected by the caller.
  @return tablespace filename created from the space_id */
  char *file_name() {
    if (m_file_name == nullptr) {
      m_file_name = make_file_name(m_id);
    }

    return (m_file_name);
  }

  /** Build a log file name based on space_id
  @param[in]	space_id	id of the undo tablespace.
  @return DB_SUCCESS or error code */
  char *make_log_file_name(space_id_t space_id);

  /** Get the undo log filename. Make it if not yet made.
  NOTE: This is only called from stack objects so there is no
  race condition. If it is ever called from a shared object
  like undo::spaces, then it must be protected by the caller.
  @return tablespace filename created from the space_id */
  char *log_file_name() {
    if (m_log_file_name == nullptr) {
      m_log_file_name = make_log_file_name(m_id);
    }

    return (m_log_file_name);
  }

  /** Get the undo tablespace ID.
  @return tablespace ID */
  space_id_t id() { return (m_id); }

  /** Get the undo tablespace number.  This is the same as m_id
  if m_id is 0 or this is a v5.6-5.7 undo tablespace. v8+ undo
  tablespaces use a space_id from the reserved range.
  @return undo tablespace number */
  space_id_t num() {
    ut_ad(m_num < FSP_MAX_ROLLBACK_SEGMENTS);

    return (m_num);
  }

  /** Get a reference to the List of rollback segments within
  this undo tablespace.
  @return a reference to the Rsegs vector. */
  Rsegs *rsegs() { return (m_rsegs); }

  /** Report whether this undo tablespace was explicitly created
  by an SQL statement.
  @return true if the tablespace was created explicitly. */
  bool is_explicit() { return (!m_implicit); }

  /** Report whether this undo tablespace was implicitly created.
  @return true if the tablespace was created implicitly. */
  bool is_implicit() { return (m_implicit); }

  /** Report whether this undo tablespace was created at startup.
  @retval true if created at startup.
  @retval false if pre-existed at startup. */
  bool is_new() { return (m_new); }

  /** Note that this undo tablespace is being created. */
  void set_new() { m_new = true; }

  /** Return whether the undo tablespace is active.
  @return true if active */
  bool is_active() {
    if (m_rsegs == nullptr) {
      return (false);
    }
    m_rsegs->s_lock();
    bool ret = m_rsegs->is_active();
    m_rsegs->s_unlock();
    return (ret);
  }

  /** Return whether the undo tablespace is active. For optimization purposes,
  do not take a latch.
  @return true if active */
  bool is_active_no_latch() {
    if (m_rsegs == nullptr) {
      return (false);
    }
    return (m_rsegs->is_active());
  }

  /** Return the rseg at the requested rseg slot if the undo space is active.
  @param[in] slot   The slot of the rseg.  1 to 127
  @return Rseg pointer of nullptr if the space is not active. */
  trx_rseg_t *get_active(ulint slot) {
    m_rsegs->s_lock();
    if (!m_rsegs->is_active()) {
      m_rsegs->s_unlock();
      return (nullptr);
    }

    /* Mark the chosen rseg so that it will not be selected
    for UNDO truncation. */
    trx_rseg_t *rseg = m_rsegs->at(slot);
    rseg->trx_ref_count++;

    m_rsegs->s_unlock();

    return (rseg);
  }

  /**
    Check if the txn rseg is the expected one.

    @params[in]   slot            The slot of rseg in the tablespace.
    @params[in]   expected_rseg   Expected rollback segment

    @retval       true if the rseg in the **slot** is matched with the
                  expect_rseg.
  */
  bool compare_rseg(ulint slot, const trx_rseg_t *expect_rseg) {
    bool match = false;
    m_rsegs->s_lock();

    ut_ad(is_txn());
    ut_ad(m_rsegs->is_active());

    trx_rseg_t *rseg = m_rsegs->at(slot);
    match = (rseg == expect_rseg);

    m_rsegs->s_unlock();
    return match;
  }

  /** Return whether the undo tablespace is inactive due to
  implicit selection by the purge thread.
  @return true if marked for truncation by the purge thread */
  bool is_inactive_implicit() {
    if (m_rsegs == nullptr) {
      return (false);
    }
    m_rsegs->s_lock();
    bool ret = m_rsegs->is_inactive_implicit();
    m_rsegs->s_unlock();
    return (ret);
  }

  /** Return whether the undo tablespace was made inactive by
  ALTER TABLESPACE.
  @return true if altered inactive */
  bool is_inactive_explicit() {
    if (m_rsegs == nullptr) {
      return (false);
    }
    m_rsegs->s_lock();
    bool ret = m_rsegs->is_inactive_explicit();
    m_rsegs->s_unlock();
    return (ret);
  }

  /** Return whether the undo tablespace is empty and ready
  to be dropped.
  @return true if empty */
  bool is_empty() {
    if (m_rsegs == nullptr) {
      return (true);
    }
    m_rsegs->s_lock();
    bool ret = m_rsegs->is_empty();
    m_rsegs->s_unlock();
    return (ret);
  }

  /** Set the undo tablespace active for use by transactions. */
  void set_active() {
    m_rsegs->x_lock();
    m_rsegs->set_active();
    m_rsegs->x_unlock();
  }

  /** Set the state of the rollback segments in this undo tablespace to
  inactive_implicit if currently active.  If the state is inactive_explicit,
  leave as is. Then put the space_id into the callers marked_space_id.
  This is done when marking a space for truncate.  It will not be used
  for new transactions until it becomes active again. */
  void set_inactive_implicit(space_id_t *marked_space_id) {
    m_rsegs->x_lock();
    if (m_rsegs->is_active()) {
      m_rsegs->set_inactive_implicit();
    }
    *marked_space_id = m_id;

    m_rsegs->x_unlock();
  }

  /** Make the undo tablespace inactive so that it will not be
  used for new transactions.  The purge thread will clear out
  all the undo logs, truncate it, and then mark it empty. */
  void set_inactive_explicit() {
    m_rsegs->x_lock();
    if (m_rsegs->is_active() || m_rsegs->is_inactive_implicit()) {
      m_rsegs->set_inactive_explicit();
    }
    m_rsegs->x_unlock();
  }

  /** Make the undo tablespace active again so that it will
  be used for new transactions.
  If current State is ___ then do:
  empty:            Set active.
  active_implicit:  Ignore.  It was not altered inactive. When it is done
                    being truncated it will go back to active.
  active_explicit:  Depends if it is marked for truncation.
    marked:         Set to inactive_implicit. the next state will be active.
    not yet:        Set to active so that it does not get truncated.  */
  void alter_active();

  /** Set the state of the undo tablespace to empty so that it
  can be dropped. */
  void set_empty() {
    m_rsegs->x_lock();
    m_rsegs->set_empty();
    m_rsegs->x_unlock();
  }

 private:
  /** Undo Tablespace ID. */
  space_id_t m_id;

  /** Undo Tablespace number, from 1 to 127. This is the
  7-bit number that is used in a rollback pointer.
  Use id2num() to get this number from a space_id. */
  space_id_t m_num;

  /** True if this is an implicit undo tablespace */
  bool m_implicit;

  /** True if this undo tablespace was implicitly created when
  this instance started up. False if it pre-existed. */
  bool m_new;

  /** The tablespace name, auto-generated when needed from
  the space number. */
  char *m_space_name;

  /** The tablespace file name, auto-generated when needed
  from the space number. */
  char *m_file_name;

  /** The tablespace log file name, auto-generated when needed
  from the space number. */
  char *m_log_file_name;

  /** List of rollback segments within this tablespace.
  This is not always used. Must call init_rsegs to use it. */
  Rsegs *m_rsegs;

  /** Lizard transaction tablespace */
  bool m_txn;

 public:
  bool is_txn() const { return m_txn; }
  void set_txn() { m_txn = true; }
};

/** List of undo tablespaces, each containing a list of
rollback segments. */
class Tablespaces {
  using Tablespaces_Vector =
      std::vector<Tablespace *, ut_allocator<Tablespace *>>;

 public:
  Tablespaces() { init(); }

  ~Tablespaces() { deinit(); }

  /** Initialize */
  void init();

  /** De-initialize */
  void deinit();

  /** Clear the contents of the list of Tablespace objects.
  This does not deallocate any memory. */
  void clear() {
    for (auto undo_space : m_spaces) {
      UT_DELETE(undo_space);
    }
    m_spaces.clear();
  }

  /** Get the number of tablespaces tracked by this object. */
  ulint size() { return (m_spaces.size()); }

  /** See if the list of tablespaces is empty. */
  bool empty() { return (m_spaces.empty()); }

  /** Get the Tablespace tracked at a position. */
  Tablespace *at(size_t pos) { return (m_spaces.at(pos)); }

  /** Get the Tablespace at back. */
  Tablespace *back() { return m_spaces.back(); }

  /** Add a new undo::Tablespace to the back of the vector.
  The vector has been pre-allocated to 128 so read threads will
  not loose what is pointed to. If tablespace_name and file_name
  are standard names, they are optional.
  @param[in]	ref_undo_space	undo tablespace */
  void add(Tablespace &ref_undo_space, int pos);

  /** Drop an existing explicit undo::Tablespace.
  @param[in]	undo_space	pointer to undo space */
  void drop(Tablespace *undo_space);

  /** Drop an existing explicit undo::Tablespace.
  @param[in]	ref_undo_space	reference to undo space */
  void drop(Tablespace &ref_undo_space);

  /** Check if the given space_id is in the vector.
  @param[in]  num  undo tablespace number
  @return true if space_id is found, else false */
  bool contains(space_id_t num) { return (find(num) != nullptr); }

  /** Lizard : mark transaction tablespace */
  void mark_txn();

  /** Find the given space_num in the vector.
  @param[in]  num  undo tablespace number
  @return pointer to an undo::Tablespace struct */
  Tablespace *find(space_id_t num) {
    if (m_spaces.empty()) {
      return (nullptr);
    }

    /* The sort method above puts this vector in order by
    Tablespace::num. If there are no gaps, then we should
    be able to find it quickly. */
    space_id_t slot = num - 1;
    if (slot < m_spaces.size()) {
      auto undo_space = m_spaces.at(slot);
      if (undo_space->num() == num) {
        return (undo_space);
      }
    }

    /* If there are gaps in the numbering, do a search. */
    for (auto undo_space : m_spaces) {
      if (undo_space->num() == num) {
        return (undo_space);
      }
    }

    return (nullptr);
  }

#ifdef UNIV_DEBUG
  /** Determine if this thread owns a lock on m_latch. */
  bool own_latch() {
    return (rw_lock_own(m_latch, RW_LOCK_X) || rw_lock_own(m_latch, RW_LOCK_S));
  }
#endif /* UNIV_DEBUG */

  /** Get a shared lock on m_spaces. */
  void s_lock() { rw_lock_s_lock(m_latch); }

  /** Release a shared lock on m_spaces. */
  void s_unlock() { rw_lock_s_unlock(m_latch); }

  /** Get an exclusive lock on m_spaces. */
  void x_lock() { rw_lock_x_lock(m_latch); }

  /** Release an exclusive lock on m_spaces. */
  void x_unlock() { rw_lock_x_unlock(m_latch); }

  Tablespaces_Vector m_spaces;

 private:
  /** RW lock to protect m_spaces.
  x for adding elements, s for scanning, size() etc. */
  rw_lock_t *m_latch;
};

/** Mutext for serializing undo tablespace related DDL.  These have to do with
creating and dropping undo tablespaces. */
extern ib_mutex_t ddl_mutex;

/** A global object that contains a vector of undo::Tablespace structs. */
extern Tablespaces *spaces;

/** Create the truncate log file. Needed to track the state of truncate during
a crash. An auxiliary redo log file undo_<space_id>_trunc.log will be created
while the truncate of the UNDO is in progress. This file is required during
recovery to complete the truncate.
@param[in]  undo_space  undo tablespace to truncate.
@return DB_SUCCESS or error code.*/
dberr_t start_logging(Tablespace *undo_space);

/** Mark completion of undo truncate action by writing magic number
to the log file and then removing it from the disk.
If we are going to remove it from disk then why write magic number?
This is to safeguard from unlink (file-system) anomalies that will
keep the link to the file even after unlink action is successful
and ref-count = 0.
@param[in]  space_num  number of the undo tablespace to truncate. */
void done_logging(space_id_t space_num);

/** Check if TRUNCATE_DDL_LOG file exist.
@param[in]  space_num  undo tablespace number
@return true if exist else false. */
bool is_active_truncate_log_present(space_id_t space_num);

/** list of undo tablespaces that need header pages and rollback
segments written to them at startup.  This can be because they are
newly initialized, were being truncated and the system crashed, or
they were an old format at startup and were replaced when they were
opened. Old format undo tablespaces do not have space_ids between
dict_sys_t::s_min_undo_space_id and dict_sys_t::s_max_undo_space_id
and they do not contain an RSEG_ARRAY page. */
extern Space_Ids s_under_construction;

/** Add undo tablespace to s_under_construction vector.
@param[in]	space_id	space id of tablespace to
truncate */
void add_space_to_construction_list(space_id_t space_id);

/** Clear the s_under_construction vector. */
void clear_construction_list();

/** Is an undo tablespace under constuction at the moment.
@param[in]	space_id	space id to check
@return true if marked for truncate, else false. */
bool is_under_construction(space_id_t space_id);

/** Set an undo tablespace active. */
void set_active(space_id_t space_id);

/* Return whether the undo tablespace is active.  If this is a
non-undo tablespace, then it will not be found in spaces and it
will not be under construction, so this function will return true.
@param[in]  space_id   Undo Tablespace ID
@param[in]  get_latch  Specifies whether the rsegs->s_lock() is needed.
@return true if active (non-undo spaces are always active) */
bool is_active(space_id_t space_id, bool get_latch = true);

/** Track an UNDO tablespace marked for truncate. */
class Truncate {
 public:
  Truncate()
      : m_space_id_marked(SPACE_UNKNOWN),
        m_purge_rseg_truncate_frequency(
            static_cast<ulint>(srv_purge_rseg_truncate_frequency)) {
    /* Do Nothing. */
  }

  /** Is tablespace selected for truncate.
  @return true if undo tablespace is marked for truncate */
  bool is_marked() const { return (m_space_id_marked != SPACE_UNKNOWN); }

  /** Mark the undo tablespace selected for truncate as empty
  so that it will be truncated next. */
  void set_marked_space_empty() { m_marked_space_is_empty = true; }

  /** Is tablespace selected for truncate empty of undo logs yet?
  @return true if the marked undo tablespace has no more undo logs */
  bool is_marked_space_empty() const { return (m_marked_space_is_empty); }

  /** Mark the tablespace for truncate.
  @param[in]  undo_space  undo tablespace to truncate. */
  void mark(Tablespace *undo_space) {
    /* Set the internal state of this undo space to inactive_implicit
    so that its rsegs will not be allocated to any new transaction.
    If the space is already in the inactive_explicit state, it will
    stay there.
    Note that the DD is not modified since in case of crash, the
    action must be completed before the DD is available.
    Set both the state and this marked id while this routine has
    an x_lock on m_rsegs because a concurrent user thread might issue
    undo_space->alter_active(). */
    undo_space->set_inactive_implicit(&m_space_id_marked);

    m_marked_space_is_empty = false;

    /* We found an UNDO-tablespace to truncate so set the
    local purge rseg truncate frequency to 3. This will help
    accelerate the purge action and in turn truncate. */
    set_rseg_truncate_frequency(3);
  }

  /** Get the ID of the tablespace marked for truncate.
  @return tablespace ID marked for truncate. */
  space_id_t get_marked_space_num() const {
    return (id2num(m_space_id_marked));
  }

  /** Reset for next rseg truncate. */
  void reset() {
    /* Sync with global value as we are done with
    truncate now. */
    set_rseg_truncate_frequency(
        static_cast<ulint>(srv_purge_rseg_truncate_frequency));

    m_marked_space_is_empty = false;
    m_space_id_marked = SPACE_UNKNOWN;
  }

  /** Get the undo tablespace number to start a scan.
  Re-adjust in case the spaces::size() went down.
  @return	UNDO space_num to start scanning. */
  space_id_t get_scan_space_num() const {
    s_scan_pos = s_scan_pos % undo::spaces->size();

    Tablespace *undo_space = undo::spaces->at(s_scan_pos);

    return (undo_space->num());
  }

  /** Increment the scanning position in a round-robin fashion.
  @return	UNDO space_num at incremented scanning position. */
  space_id_t increment_scan() const {
    /** Round-robin way of selecting an undo tablespace
    for the truncate operation. Once we reach the end of
    the list of known undo tablespace IDs, move back to
    the first undo tablespace ID. This will scan active
    as well as inactive undo tablespaces. */
    s_scan_pos = (s_scan_pos + 1) % undo::spaces->size();

    return (get_scan_space_num());
  }

  /** Get local rseg purge truncate frequency
  @return rseg purge truncate frequency. */
  ulint get_rseg_truncate_frequency() const {
    return (m_purge_rseg_truncate_frequency);
  }

  /** Set local rseg purge truncate frequency */
  void set_rseg_truncate_frequency(ulint frequency) {
    m_purge_rseg_truncate_frequency = frequency;
  }

 private:
  /** UNDO space ID that is marked for truncate. */
  space_id_t m_space_id_marked;

  /** This is true if the marked space is empty of undo logs
  and ready to truncate.  We leave the rsegs object 'inactive'
  until after it is truncated and rebuilt.  This allow the
  code to do the check for undo logs only once. */
  bool m_marked_space_is_empty;

  /** Rollback segment(s) purge frequency. This is a local
  value maintained along with the global value. It is set
  to the global value in the before each truncate.  But when
  a tablespace is marked for truncate it is updated to 1 and
  then minimum value among 2 is used by the purge action. */
  ulint m_purge_rseg_truncate_frequency;

  /** Start scanning for UNDO tablespace from this
  vector position. This is to avoid bias selection
  of one tablespace always. */
  static size_t s_scan_pos;

}; /* class Truncate */

} /* namespace undo */

/** The control structure used in the purge operation */
struct trx_purge_t {
  sess_t *sess; /*!< System session running the purge
                query */
  trx_t *trx;   /*!< System transaction running the
                purge query: this trx is not in the
                trx list of the trx system and it
                never ends */
#ifndef UNIV_HOTBACKUP
  rw_lock_t latch;              /*!< The latch protecting the purge
                                view. A purge operation must acquire an
                                x-latch here for the instant at which
                                it changes the purge view: an undo
                                log operation can prevent this by
                                obtaining an s-latch here. It also
                                protects state and running */
#endif                          /* !UNIV_HOTBACKUP */
  os_event_t event;             /*!< State signal event */
  ulint n_stop;                 /*!< Counter to track number stops */
  volatile bool running;        /*!< true, if purge is active,
                                we check this without the latch too */
  volatile purge_state_t state; /*!< Purge coordinator thread states,
                                we check this in several places
                                without holding the latch. */
  que_t *query;                 /*!< The query graph which will do the
                                parallelized purge operation */

  lizard::Vision vision; /*!< The purge will not remove undo logs
                          which are > this vision(purge vision) */

  bool view_active;             /*!< true if view is active */
  volatile ulint n_submitted;   /*!< Count of total tasks submitted
                                to the task queue */
  volatile ulint n_completed;   /*!< Count of total tasks completed */

  /*------------------------------*/
  /* The following two fields form the 'purge pointer' which advances
  during a purge, and which is used in history list truncation */

  purge_iter_t iter;  /* Limit up to which we have read and
                      parsed the UNDO log records.  Not
                      necessarily purged from the indexes.
                      Note that this can never be less than
                      the limit below, we check for this
                      invariant in trx0purge.cc */
  purge_iter_t limit; /* The 'purge pointer' which advances
                      during a purge, and which is used in
                      history list truncation */
#ifdef UNIV_DEBUG
  purge_iter_t done; /* Indicate 'purge pointer' which have
                     purged already accurately. */
#endif               /* UNIV_DEBUG */
  /*-----------------------------*/
  ibool next_stored;     /*!< TRUE if the info of the next record
                         to purge is stored below: if yes, then
                         the transaction number and the undo
                         number of the record are stored in
                         purge_trx_no and purge_undo_no above */
  trx_rseg_t *rseg;      /*!< Rollback segment for the next undo
                         record to purge */
  page_no_t page_no;     /*!< Page number for the next undo
                         record to purge, page number of the
                         log header, if dummy record */
  ulint offset;          /*!< Page offset for the next undo
                         record to purge, 0 if the dummy
                         record */
  page_no_t hdr_page_no; /*!< Header page of the undo log where
                         the next record to purge belongs */
  ulint hdr_offset;      /*!< Header byte offset on the page */

  lizard::TxnUndoRsegsIterator *rseg_iter; /*!< Iterator to get the next rseg
                                           to process */

  lizard::purge_heap_t *purge_heap; /*!< Binary min-heap, ordered on
                                    TxnUndoRsegs::scn. It is protected
                                    by the pq_mutex */

  PQMutex pq_mutex;        /*!< Mutex protecting purge_queue */

  undo::Truncate undo_trunc; /*!< Track UNDO tablespace marked
                             for truncate. */

  mem_heap_t *heap; /*!< Heap for reading the undo log
                    records */

  /** All transactions whose scn <= purged_scn must have been purged.
  Only the purge sys coordinator thread and recover thread can modify it. */
  std::atomic<scn_t> purged_scn;

  utc_t top_undo_utc;

  /** Similar with purged_scn */
  Purged_gcn purged_gcn;
};

#include "trx0purge.ic"

#endif /* trx0purge_h */
