/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_BTREE_UPDATE_H
#define _BCACHEFS_BTREE_UPDATE_H

#include "btree_iter.h"
#include "journal.h"

struct bch_fs;
struct btree;

void bch2_btree_node_lock_for_insert(struct bch_fs *, struct btree *,
				     struct btree_iter *);
bool bch2_btree_bset_insert_key(struct btree_iter *, struct btree *,
				struct btree_node_iter *, struct bkey_i *);
void bch2_btree_journal_key(struct btree_trans *, struct btree_iter *,
			    struct bkey_i *);

enum {
	__BTREE_INSERT_ATOMIC,
	__BTREE_INSERT_NOUNLOCK,
	__BTREE_INSERT_NOFAIL,
	__BTREE_INSERT_NOCHECK_RW,
	__BTREE_INSERT_LAZY_RW,
	__BTREE_INSERT_USE_RESERVE,
	__BTREE_INSERT_USE_ALLOC_RESERVE,
	__BTREE_INSERT_JOURNAL_REPLAY,
	__BTREE_INSERT_JOURNAL_RESERVED,
	__BTREE_INSERT_NOMARK_OVERWRITES,
	__BTREE_INSERT_NOMARK,
	__BTREE_INSERT_NO_CLEAR_REPLICAS,
	__BTREE_INSERT_BUCKET_INVALIDATE,
	__BTREE_INSERT_NOWAIT,
	__BTREE_INSERT_GC_LOCK_HELD,
	__BCH_HASH_SET_MUST_CREATE,
	__BCH_HASH_SET_MUST_REPLACE,
};

/*
 * Don't drop/retake locks before doing btree update, instead return -EINTR if
 * we had to drop locks for any reason
 */
#define BTREE_INSERT_ATOMIC		(1 << __BTREE_INSERT_ATOMIC)

/*
 * Don't drop locks _after_ successfully updating btree:
 */
#define BTREE_INSERT_NOUNLOCK		(1 << __BTREE_INSERT_NOUNLOCK)

/* Don't check for -ENOSPC: */
#define BTREE_INSERT_NOFAIL		(1 << __BTREE_INSERT_NOFAIL)

#define BTREE_INSERT_NOCHECK_RW		(1 << __BTREE_INSERT_NOCHECK_RW)
#define BTREE_INSERT_LAZY_RW		(1 << __BTREE_INSERT_LAZY_RW)

/* for copygc, or when merging btree nodes */
#define BTREE_INSERT_USE_RESERVE	(1 << __BTREE_INSERT_USE_RESERVE)
#define BTREE_INSERT_USE_ALLOC_RESERVE	(1 << __BTREE_INSERT_USE_ALLOC_RESERVE)

/* Insert is for journal replay - don't get journal reservations: */
#define BTREE_INSERT_JOURNAL_REPLAY	(1 << __BTREE_INSERT_JOURNAL_REPLAY)

#define BTREE_INSERT_JOURNAL_RESERVED	(1 << __BTREE_INSERT_JOURNAL_RESERVED)

/* Don't mark overwrites, just new key: */
#define BTREE_INSERT_NOMARK_OVERWRITES	(1 << __BTREE_INSERT_NOMARK_OVERWRITES)

/* Don't call mark new key at all: */
#define BTREE_INSERT_NOMARK		(1 << __BTREE_INSERT_NOMARK)

#define BTREE_INSERT_NO_CLEAR_REPLICAS	(1 << __BTREE_INSERT_NO_CLEAR_REPLICAS)

#define BTREE_INSERT_BUCKET_INVALIDATE	(1 << __BTREE_INSERT_BUCKET_INVALIDATE)

/* Don't block on allocation failure (for new btree nodes: */
#define BTREE_INSERT_NOWAIT		(1 << __BTREE_INSERT_NOWAIT)
#define BTREE_INSERT_GC_LOCK_HELD	(1 << __BTREE_INSERT_GC_LOCK_HELD)

#define BCH_HASH_SET_MUST_CREATE	(1 << __BCH_HASH_SET_MUST_CREATE)
#define BCH_HASH_SET_MUST_REPLACE	(1 << __BCH_HASH_SET_MUST_REPLACE)

int bch2_btree_delete_at(struct btree_trans *, struct btree_iter *, unsigned);

int bch2_btree_insert(struct bch_fs *, enum btree_id, struct bkey_i *,
		     struct disk_reservation *, u64 *, int flags);

int bch2_btree_delete_at_range(struct btree_trans *, struct btree_iter *,
			       struct bpos, u64 *);
int bch2_btree_delete_range(struct bch_fs *, enum btree_id,
			    struct bpos, struct bpos, u64 *);

int bch2_btree_node_rewrite(struct bch_fs *c, struct btree_iter *,
			    __le64, unsigned);
int bch2_btree_node_update_key(struct bch_fs *, struct btree_iter *,
			       struct btree *, struct bkey_i_btree_ptr *);

int __bch2_trans_commit(struct btree_trans *);

/**
 * bch2_trans_commit - insert keys at given iterator positions
 *
 * This is main entry point for btree updates.
 *
 * Return values:
 * -EINTR: locking changed, this function should be called again. Only returned
 *  if passed BTREE_INSERT_ATOMIC.
 * -EROFS: filesystem read only
 * -EIO: journal or btree node IO error
 */
static inline int bch2_trans_commit(struct btree_trans *trans,
				    struct disk_reservation *disk_res,
				    u64 *journal_seq,
				    unsigned flags)
{
	trans->disk_res		= disk_res;
	trans->journal_seq	= journal_seq;
	trans->flags		= flags;

	return __bch2_trans_commit(trans);
}

static inline void bch2_trans_update(struct btree_trans *trans,
				     struct btree_iter *iter,
				     struct bkey_i *k)
{
	EBUG_ON(trans->nr_updates >= trans->nr_iters + 4);

	iter->flags |= BTREE_ITER_KEEP_UNTIL_COMMIT;

	trans->updates[trans->nr_updates++] = (struct btree_insert_entry) {
		.iter = iter, .k = k
	};
}

#define bch2_trans_do(_c, _journal_seq, _flags, _do)			\
({									\
	struct btree_trans trans;					\
	int _ret;							\
									\
	bch2_trans_init(&trans, (_c), 0, 0);				\
									\
	do {								\
		bch2_trans_begin(&trans);				\
									\
		_ret = (_do) ?:	bch2_trans_commit(&trans, NULL,		\
					(_journal_seq), (_flags));	\
	} while (_ret == -EINTR);					\
									\
	bch2_trans_exit(&trans);					\
	_ret;								\
})

#define trans_for_each_update(_trans, _i)				\
	for ((_i) = (_trans)->updates;					\
	     (_i) < (_trans)->updates + (_trans)->nr_updates;		\
	     (_i)++)

#endif /* _BCACHEFS_BTREE_UPDATE_H */
