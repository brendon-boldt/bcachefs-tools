// SPDX-License-Identifier: GPL-2.0

#include "bcachefs.h"
#include "bkey_methods.h"
#include "btree_update.h"
#include "extents.h"
#include "fs.h"
#include "rebalance.h"
#include "str_hash.h"
#include "xattr.h"

#include <linux/dcache.h>
#include <linux/posix_acl_xattr.h>
#include <linux/xattr.h>

static const struct xattr_handler *bch2_xattr_type_to_handler(unsigned);

static u64 bch2_xattr_hash(const struct bch_hash_info *info,
			  const struct xattr_search_key *key)
{
	struct bch_str_hash_ctx ctx;

	bch2_str_hash_init(&ctx, info);
	bch2_str_hash_update(&ctx, info, &key->type, sizeof(key->type));
	bch2_str_hash_update(&ctx, info, key->name.name, key->name.len);

	return bch2_str_hash_end(&ctx, info);
}

static u64 xattr_hash_key(const struct bch_hash_info *info, const void *key)
{
	return bch2_xattr_hash(info, key);
}

static u64 xattr_hash_bkey(const struct bch_hash_info *info, struct bkey_s_c k)
{
	struct bkey_s_c_xattr x = bkey_s_c_to_xattr(k);

	return bch2_xattr_hash(info,
		 &X_SEARCH(x.v->x_type, x.v->x_name, x.v->x_name_len));
}

static bool xattr_cmp_key(struct bkey_s_c _l, const void *_r)
{
	struct bkey_s_c_xattr l = bkey_s_c_to_xattr(_l);
	const struct xattr_search_key *r = _r;

	return l.v->x_type != r->type ||
		l.v->x_name_len != r->name.len ||
		memcmp(l.v->x_name, r->name.name, r->name.len);
}

static bool xattr_cmp_bkey(struct bkey_s_c _l, struct bkey_s_c _r)
{
	struct bkey_s_c_xattr l = bkey_s_c_to_xattr(_l);
	struct bkey_s_c_xattr r = bkey_s_c_to_xattr(_r);

	return l.v->x_type != r.v->x_type ||
		l.v->x_name_len != r.v->x_name_len ||
		memcmp(l.v->x_name, r.v->x_name, r.v->x_name_len);
}

const struct bch_hash_desc bch2_xattr_hash_desc = {
	.btree_id	= BTREE_ID_XATTRS,
	.key_type	= KEY_TYPE_xattr,
	.hash_key	= xattr_hash_key,
	.hash_bkey	= xattr_hash_bkey,
	.cmp_key	= xattr_cmp_key,
	.cmp_bkey	= xattr_cmp_bkey,
};

const char *bch2_xattr_invalid(const struct bch_fs *c, struct bkey_s_c k)
{
	const struct xattr_handler *handler;
	struct bkey_s_c_xattr xattr = bkey_s_c_to_xattr(k);

	if (bkey_val_bytes(k.k) < sizeof(struct bch_xattr))
		return "value too small";

	if (bkey_val_u64s(k.k) <
	    xattr_val_u64s(xattr.v->x_name_len,
			   le16_to_cpu(xattr.v->x_val_len)))
		return "value too small";

	if (bkey_val_u64s(k.k) >
	    xattr_val_u64s(xattr.v->x_name_len,
			   le16_to_cpu(xattr.v->x_val_len) + 4))
		return "value too big";

	handler = bch2_xattr_type_to_handler(xattr.v->x_type);
	if (!handler)
		return "invalid type";

	if (memchr(xattr.v->x_name, '\0', xattr.v->x_name_len))
		return "xattr name has invalid characters";

	return NULL;
}

void bch2_xattr_to_text(struct printbuf *out, struct bch_fs *c,
			struct bkey_s_c k)
{
	const struct xattr_handler *handler;
	struct bkey_s_c_xattr xattr = bkey_s_c_to_xattr(k);

	handler = bch2_xattr_type_to_handler(xattr.v->x_type);
	if (handler && handler->prefix)
		pr_buf(out, "%s", handler->prefix);
	else if (handler)
		pr_buf(out, "(type %u)", xattr.v->x_type);
	else
		pr_buf(out, "(unknown type %u)", xattr.v->x_type);

	bch_scnmemcpy(out, xattr.v->x_name,
		      xattr.v->x_name_len);
	pr_buf(out, ":");
	bch_scnmemcpy(out, xattr_val(xattr.v),
		      le16_to_cpu(xattr.v->x_val_len));
}

int bch2_xattr_get(struct bch_fs *c, struct bch_inode_info *inode,
		   const char *name, void *buffer, size_t size, int type)
{
	struct btree_trans trans;
	struct btree_iter *iter;
	struct bkey_s_c_xattr xattr;
	int ret;

	bch2_trans_init(&trans, c, 0, 0);

	iter = bch2_hash_lookup(&trans, bch2_xattr_hash_desc,
				&inode->ei_str_hash, inode->v.i_ino,
				&X_SEARCH(type, name, strlen(name)),
				0);
	if (IS_ERR(iter)) {
		bch2_trans_exit(&trans);
		BUG_ON(PTR_ERR(iter) == -EINTR);

		return PTR_ERR(iter) == -ENOENT ? -ENODATA : PTR_ERR(iter);
	}

	xattr = bkey_s_c_to_xattr(bch2_btree_iter_peek_slot(iter));
	ret = le16_to_cpu(xattr.v->x_val_len);
	if (buffer) {
		if (ret > size)
			ret = -ERANGE;
		else
			memcpy(buffer, xattr_val(xattr.v), ret);
	}

	bch2_trans_exit(&trans);
	return ret;
}

int bch2_xattr_set(struct btree_trans *trans, u64 inum,
		   const struct bch_hash_info *hash_info,
		   const char *name, const void *value, size_t size,
		   int type, int flags)
{
	int ret;

	if (value) {
		struct bkey_i_xattr *xattr;
		unsigned namelen = strlen(name);
		unsigned u64s = BKEY_U64s +
			xattr_val_u64s(namelen, size);

		if (u64s > U8_MAX)
			return -ERANGE;

		xattr = bch2_trans_kmalloc(trans, u64s * sizeof(u64));
		if (IS_ERR(xattr))
			return PTR_ERR(xattr);

		bkey_xattr_init(&xattr->k_i);
		xattr->k.u64s		= u64s;
		xattr->v.x_type		= type;
		xattr->v.x_name_len	= namelen;
		xattr->v.x_val_len	= cpu_to_le16(size);
		memcpy(xattr->v.x_name, name, namelen);
		memcpy(xattr_val(&xattr->v), value, size);

		ret = bch2_hash_set(trans, bch2_xattr_hash_desc, hash_info,
			      inum, &xattr->k_i,
			      (flags & XATTR_CREATE ? BCH_HASH_SET_MUST_CREATE : 0)|
			      (flags & XATTR_REPLACE ? BCH_HASH_SET_MUST_REPLACE : 0));
	} else {
		struct xattr_search_key search =
			X_SEARCH(type, name, strlen(name));

		ret = bch2_hash_delete(trans, bch2_xattr_hash_desc,
				       hash_info, inum, &search);
	}

	if (ret == -ENOENT)
		ret = flags & XATTR_REPLACE ? -ENODATA : 0;

	return ret;
}

struct xattr_buf {
	char		*buf;
	size_t		len;
	size_t		used;
};

static int __bch2_xattr_emit(const char *prefix,
			     const char *name, size_t name_len,
			     struct xattr_buf *buf)
{
	const size_t prefix_len = strlen(prefix);
	const size_t total_len = prefix_len + name_len + 1;

	if (buf->buf) {
		if (buf->used + total_len > buf->len)
			return -ERANGE;

		memcpy(buf->buf + buf->used, prefix, prefix_len);
		memcpy(buf->buf + buf->used + prefix_len,
		       name, name_len);
		buf->buf[buf->used + prefix_len + name_len] = '\0';
	}

	buf->used += total_len;
	return 0;
}

static int bch2_xattr_emit(struct dentry *dentry,
			    const struct bch_xattr *xattr,
			    struct xattr_buf *buf)
{
	const struct xattr_handler *handler =
		bch2_xattr_type_to_handler(xattr->x_type);

	return handler && (!handler->list || handler->list(dentry))
		? __bch2_xattr_emit(handler->prefix ?: handler->name,
				    xattr->x_name, xattr->x_name_len, buf)
		: 0;
}

static int bch2_xattr_list_bcachefs(struct bch_fs *c,
				    struct bch_inode_info *inode,
				    struct xattr_buf *buf,
				    bool all)
{
	const char *prefix = all ? "bcachefs_effective." : "bcachefs.";
	unsigned id;
	int ret = 0;
	u64 v;

	for (id = 0; id < Inode_opt_nr; id++) {
		v = bch2_inode_opt_get(&inode->ei_inode, id);
		if (!v)
			continue;

		if (!all &&
		    !(inode->ei_inode.bi_fields_set & (1 << id)))
			continue;

		ret = __bch2_xattr_emit(prefix, bch2_inode_opts[id],
					strlen(bch2_inode_opts[id]), buf);
		if (ret)
			break;
	}

	return ret;
}

ssize_t bch2_xattr_list(struct dentry *dentry, char *buffer, size_t buffer_size)
{
	struct bch_fs *c = dentry->d_sb->s_fs_info;
	struct bch_inode_info *inode = to_bch_ei(dentry->d_inode);
	struct btree_trans trans;
	struct btree_iter *iter;
	struct bkey_s_c k;
	struct xattr_buf buf = { .buf = buffer, .len = buffer_size };
	u64 inum = dentry->d_inode->i_ino;
	int ret;

	bch2_trans_init(&trans, c, 0, 0);

	for_each_btree_key(&trans, iter, BTREE_ID_XATTRS,
			   POS(inum, 0), 0, k, ret) {
		BUG_ON(k.k->p.inode < inum);

		if (k.k->p.inode > inum)
			break;

		if (k.k->type != KEY_TYPE_xattr)
			continue;

		ret = bch2_xattr_emit(dentry, bkey_s_c_to_xattr(k).v, &buf);
		if (ret)
			break;
	}
	ret = bch2_trans_exit(&trans) ?: ret;

	if (ret)
		return ret;

	ret = bch2_xattr_list_bcachefs(c, inode, &buf, false);
	if (ret)
		return ret;

	ret = bch2_xattr_list_bcachefs(c, inode, &buf, true);
	if (ret)
		return ret;

	return buf.used;
}

static int bch2_xattr_get_handler(const struct xattr_handler *handler,
				  struct dentry *dentry, struct inode *vinode,
				  const char *name, void *buffer, size_t size)
{
	struct bch_inode_info *inode = to_bch_ei(vinode);
	struct bch_fs *c = inode->v.i_sb->s_fs_info;

	return bch2_xattr_get(c, inode, name, buffer, size, handler->flags);
}

static int bch2_xattr_set_handler(const struct xattr_handler *handler,
				  struct dentry *dentry, struct inode *vinode,
				  const char *name, const void *value,
				  size_t size, int flags)
{
	struct bch_inode_info *inode = to_bch_ei(vinode);
	struct bch_fs *c = inode->v.i_sb->s_fs_info;

	return bch2_trans_do(c, &inode->ei_journal_seq, BTREE_INSERT_ATOMIC,
			bch2_xattr_set(&trans, inode->v.i_ino,
				       &inode->ei_str_hash,
				       name, value, size,
				       handler->flags, flags));
}

static const struct xattr_handler bch_xattr_user_handler = {
	.prefix	= XATTR_USER_PREFIX,
	.get	= bch2_xattr_get_handler,
	.set	= bch2_xattr_set_handler,
	.flags	= KEY_TYPE_XATTR_INDEX_USER,
};

static bool bch2_xattr_trusted_list(struct dentry *dentry)
{
	return capable(CAP_SYS_ADMIN);
}

static const struct xattr_handler bch_xattr_trusted_handler = {
	.prefix	= XATTR_TRUSTED_PREFIX,
	.list	= bch2_xattr_trusted_list,
	.get	= bch2_xattr_get_handler,
	.set	= bch2_xattr_set_handler,
	.flags	= KEY_TYPE_XATTR_INDEX_TRUSTED,
};

static const struct xattr_handler bch_xattr_security_handler = {
	.prefix	= XATTR_SECURITY_PREFIX,
	.get	= bch2_xattr_get_handler,
	.set	= bch2_xattr_set_handler,
	.flags	= KEY_TYPE_XATTR_INDEX_SECURITY,
};

#ifndef NO_BCACHEFS_FS

static int opt_to_inode_opt(int id)
{
	switch (id) {
#define x(name, ...)				\
	case Opt_##name: return Inode_opt_##name;
	BCH_INODE_OPTS()
#undef  x
	default:
		return -1;
	}
}

static int __bch2_xattr_bcachefs_get(const struct xattr_handler *handler,
				struct dentry *dentry, struct inode *vinode,
				const char *name, void *buffer, size_t size,
				bool all)
{
	struct bch_inode_info *inode = to_bch_ei(vinode);
	struct bch_fs *c = inode->v.i_sb->s_fs_info;
	struct bch_opts opts =
		bch2_inode_opts_to_opts(bch2_inode_opts_get(&inode->ei_inode));
	const struct bch_option *opt;
	int id, inode_opt_id;
	char buf[512];
	struct printbuf out = PBUF(buf);
	unsigned val_len;
	u64 v;

	id = bch2_opt_lookup(name);
	if (id < 0 || !bch2_opt_is_inode_opt(id))
		return -EINVAL;

	inode_opt_id = opt_to_inode_opt(id);
	if (inode_opt_id < 0)
		return -EINVAL;

	opt = bch2_opt_table + id;

	if (!bch2_opt_defined_by_id(&opts, id))
		return -ENODATA;

	if (!all &&
	    !(inode->ei_inode.bi_fields_set & (1 << inode_opt_id)))
		return -ENODATA;

	v = bch2_opt_get_by_id(&opts, id);
	bch2_opt_to_text(&out, c, opt, v, 0);

	val_len = out.pos - buf;

	if (buffer && val_len > size)
		return -ERANGE;

	if (buffer)
		memcpy(buffer, buf, val_len);
	return val_len;
}

static int bch2_xattr_bcachefs_get(const struct xattr_handler *handler,
				   struct dentry *dentry, struct inode *vinode,
				   const char *name, void *buffer, size_t size)
{
	return __bch2_xattr_bcachefs_get(handler, dentry, vinode,
					 name, buffer, size, false);
}

struct inode_opt_set {
	int			id;
	u64			v;
	bool			defined;
};

static int inode_opt_set_fn(struct bch_inode_info *inode,
			    struct bch_inode_unpacked *bi,
			    void *p)
{
	struct inode_opt_set *s = p;

	if (s->defined)
		bi->bi_fields_set |= 1U << s->id;
	else
		bi->bi_fields_set &= ~(1U << s->id);

	bch2_inode_opt_set(bi, s->id, s->v);

	return 0;
}

static int bch2_xattr_bcachefs_set(const struct xattr_handler *handler,
				   struct dentry *dentry, struct inode *vinode,
				   const char *name, const void *value,
				   size_t size, int flags)
{
	struct bch_inode_info *inode = to_bch_ei(vinode);
	struct bch_fs *c = inode->v.i_sb->s_fs_info;
	const struct bch_option *opt;
	char *buf;
	struct inode_opt_set s;
	int opt_id, inode_opt_id, ret;

	opt_id = bch2_opt_lookup(name);
	if (opt_id < 0)
		return -EINVAL;

	opt = bch2_opt_table + opt_id;

	inode_opt_id = opt_to_inode_opt(opt_id);
	if (inode_opt_id < 0)
		return -EINVAL;

	s.id = inode_opt_id;

	if (value) {
		u64 v = 0;

		buf = kmalloc(size + 1, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		memcpy(buf, value, size);
		buf[size] = '\0';

		ret = bch2_opt_parse(c, opt, buf, &v);
		kfree(buf);

		if (ret < 0)
			return ret;

		ret = bch2_opt_check_may_set(c, opt_id, v);
		if (ret < 0)
			return ret;

		s.v = v + 1;
		s.defined = true;
	} else {
		if (!IS_ROOT(dentry)) {
			struct bch_inode_info *dir =
				to_bch_ei(d_inode(dentry->d_parent));

			s.v = bch2_inode_opt_get(&dir->ei_inode, inode_opt_id);
		} else {
			s.v = 0;
		}

		s.defined = false;
	}

	mutex_lock(&inode->ei_update_lock);
	if (inode_opt_id == Inode_opt_project) {
		ret = bch2_set_projid(c, inode, s.v);
		if (ret)
			goto err;
	}

	ret = bch2_write_inode(c, inode, inode_opt_set_fn, &s, 0);
err:
	mutex_unlock(&inode->ei_update_lock);

	if (value &&
	    (opt_id == Opt_background_compression ||
	     opt_id == Opt_background_target))
		bch2_rebalance_add_work(c, inode->v.i_blocks);

	return ret;
}

static const struct xattr_handler bch_xattr_bcachefs_handler = {
	.prefix	= "bcachefs.",
	.get	= bch2_xattr_bcachefs_get,
	.set	= bch2_xattr_bcachefs_set,
};

static int bch2_xattr_bcachefs_get_effective(
				const struct xattr_handler *handler,
				struct dentry *dentry, struct inode *vinode,
				const char *name, void *buffer, size_t size)
{
	return __bch2_xattr_bcachefs_get(handler, dentry, vinode,
					 name, buffer, size, true);
}

static const struct xattr_handler bch_xattr_bcachefs_effective_handler = {
	.prefix	= "bcachefs_effective.",
	.get	= bch2_xattr_bcachefs_get_effective,
	.set	= bch2_xattr_bcachefs_set,
};

#endif /* NO_BCACHEFS_FS */

const struct xattr_handler *bch2_xattr_handlers[] = {
	&bch_xattr_user_handler,
	&posix_acl_access_xattr_handler,
	&posix_acl_default_xattr_handler,
	&bch_xattr_trusted_handler,
	&bch_xattr_security_handler,
#ifndef NO_BCACHEFS_FS
	&bch_xattr_bcachefs_handler,
	&bch_xattr_bcachefs_effective_handler,
#endif
	NULL
};

static const struct xattr_handler *bch_xattr_handler_map[] = {
	[KEY_TYPE_XATTR_INDEX_USER]			= &bch_xattr_user_handler,
	[KEY_TYPE_XATTR_INDEX_POSIX_ACL_ACCESS]	=
		&posix_acl_access_xattr_handler,
	[KEY_TYPE_XATTR_INDEX_POSIX_ACL_DEFAULT]	=
		&posix_acl_default_xattr_handler,
	[KEY_TYPE_XATTR_INDEX_TRUSTED]		= &bch_xattr_trusted_handler,
	[KEY_TYPE_XATTR_INDEX_SECURITY]		= &bch_xattr_security_handler,
};

static const struct xattr_handler *bch2_xattr_type_to_handler(unsigned type)
{
	return type < ARRAY_SIZE(bch_xattr_handler_map)
		? bch_xattr_handler_map[type]
		: NULL;
}
