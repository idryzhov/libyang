/**
 * @file tree_schema_helpers.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief Parsing and validation helper functions for schema trees
 *
 * Copyright (c) 2015 - 2018 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE

#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "compat.h"
#include "common.h"
#include "context.h"
#include "hash_table.h"
#include "log.h"
#include "parser.h"
#include "parser_schema.h"
#include "parser_internal.h"
#include "set.h"
#include "tree.h"
#include "tree_schema.h"
#include "tree_schema_internal.h"

LY_ERR
lysc_resolve_schema_nodeid(struct lysc_ctx *ctx, const char *nodeid, size_t nodeid_len, const struct lysc_node *context_node,
        const struct lys_module *context_module, uint16_t nodetype, const struct lysc_node **target, uint16_t *result_flag)
{
    LY_ERR ret = LY_EVALID;
    const char *name, *prefix, *id;
    size_t name_len, prefix_len;
    const struct lys_module *mod;
    const char *nodeid_type;
    uint32_t getnext_extra_flag = 0;
    uint16_t current_nodetype = 0;

    assert(nodeid);
    assert(target);
    assert(result_flag);
    *target = NULL;
    *result_flag = 0;

    id = nodeid;

    if (context_node) {
        /* descendant-schema-nodeid */
        nodeid_type = "descendant";

        if (*id == '/') {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                   "Invalid descendant-schema-nodeid value \"%.*s\" - absolute-schema-nodeid used.",
                   nodeid_len ? nodeid_len : strlen(nodeid), nodeid);
            return LY_EVALID;
        }
    } else {
        /* absolute-schema-nodeid */
        nodeid_type = "absolute";

        if (*id != '/') {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                   "Invalid absolute-schema-nodeid value \"%.*s\" - missing starting \"/\".",
                   nodeid_len ? nodeid_len : strlen(nodeid), nodeid);
            return LY_EVALID;
        }
        ++id;
    }

    while (*id && (ret = ly_parse_nodeid(&id, &prefix, &prefix_len, &name, &name_len)) == LY_SUCCESS) {
        if (prefix) {
            mod = lys_module_find_prefix(context_module, prefix, prefix_len);
            if (!mod) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                       "Invalid %s-schema-nodeid value \"%.*s\" - prefix \"%.*s\" not defined in module \"%s\".",
                       nodeid_type, id - nodeid, nodeid, prefix_len, prefix, context_module->name);
                return LY_ENOTFOUND;
            }
        } else {
            mod = context_module;
        }
        if (context_node && (context_node->nodetype & (LYS_RPC | LYS_ACTION))) {
            /* move through input/output manually */
            if (mod != context_node->module) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                       "Invalid %s-schema-nodeid value \"%.*s\" - target node not found.", nodeid_type, id - nodeid, nodeid);
                return LY_ENOTFOUND;
            }
            if (!ly_strncmp("input", name, name_len)) {
                context_node = (struct lysc_node *)&((struct lysc_action *)context_node)->input;
            } else if (!ly_strncmp("output", name, name_len)) {
                context_node = (struct lysc_node *)&((struct lysc_action *)context_node)->output;
                getnext_extra_flag = LYS_GETNEXT_OUTPUT;
            } else {
                /* only input or output is valid */
                context_node = NULL;
            }
        } else {
            context_node = lys_find_child(context_node, mod, name, name_len, 0,
                    getnext_extra_flag | LYS_GETNEXT_NOSTATECHECK | LYS_GETNEXT_WITHCHOICE | LYS_GETNEXT_WITHCASE);
            getnext_extra_flag = 0;
        }
        if (!context_node) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                    "Invalid %s-schema-nodeid value \"%.*s\" - target node not found.", nodeid_type, id - nodeid, nodeid);
            return LY_ENOTFOUND;
        }
        current_nodetype = context_node->nodetype;

        if (current_nodetype == LYS_NOTIF) {
            (*result_flag) |= LYSC_OPT_NOTIFICATION;
        } else if (current_nodetype == LYS_INPUT) {
            (*result_flag) |= LYSC_OPT_RPC_INPUT;
        } else if (current_nodetype == LYS_OUTPUT) {
            (*result_flag) |= LYSC_OPT_RPC_OUTPUT;
        }

        if (!*id || (nodeid_len && ((size_t)(id - nodeid) >= nodeid_len))) {
            break;
        }
        if (*id != '/') {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                   "Invalid %s-schema-nodeid value \"%.*s\" - missing \"/\" as node-identifier separator.",
                   nodeid_type, id - nodeid + 1, nodeid);
            return LY_EVALID;
        }
        ++id;
    }

    if (ret == LY_SUCCESS) {
        *target = context_node;
        if (nodetype && !(current_nodetype & nodetype)) {
            return LY_EDENIED;
        }
    } else {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
               "Invalid %s-schema-nodeid value \"%.*s\" - unexpected end of expression.",
               nodeid_type, nodeid_len ? nodeid_len : strlen(nodeid), nodeid);
    }

    return ret;
}

LY_ERR
lysp_check_prefix(struct lys_parser_ctx *ctx, struct lysp_import *imports, const char *module_prefix, const char **value)
{
    struct lysp_import *i;

    if (module_prefix && &module_prefix != value && !strcmp(module_prefix, *value)) {
        LOGVAL_PARSER(ctx, LYVE_REFERENCE, "Prefix \"%s\" already used as module prefix.", *value);
        return LY_EEXIST;
    }
    LY_ARRAY_FOR(imports, struct lysp_import, i) {
        if (i->prefix && &i->prefix != value && !strcmp(i->prefix, *value)) {
            LOGVAL_PARSER(ctx, LYVE_REFERENCE, "Prefix \"%s\" already used to import \"%s\" module.", *value, i->name);
            return LY_EEXIST;
        }
    }
    return LY_SUCCESS;
}

LY_ERR
lysc_check_status(struct lysc_ctx *ctx,
        uint16_t flags1, void *mod1, const char *name1,
        uint16_t flags2, void *mod2, const char *name2)
{
    uint16_t flg1, flg2;

    flg1 = (flags1 & LYS_STATUS_MASK) ? (flags1 & LYS_STATUS_MASK) : LYS_STATUS_CURR;
    flg2 = (flags2 & LYS_STATUS_MASK) ? (flags2 & LYS_STATUS_MASK) : LYS_STATUS_CURR;

    if ((flg1 < flg2) && (mod1 == mod2)) {
        if (ctx) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                   "A %s definition \"%s\" is not allowed to reference %s definition \"%s\".",
                   flg1 == LYS_STATUS_CURR ? "current" : "deprecated", name1,
                   flg2 == LYS_STATUS_OBSLT ? "obsolete" : "deprecated", name2);
        }
        return LY_EVALID;
    }

    return LY_SUCCESS;
}

LY_ERR
lysp_check_date(struct lys_parser_ctx *ctx, const char *date, uint8_t date_len, const char *stmt)
{
    struct tm tm, tm_;
    char *r;

    LY_CHECK_ARG_RET(ctx ? PARSER_CTX(ctx) : NULL, date, LY_EINVAL);
    LY_CHECK_ERR_RET(date_len != LY_REV_SIZE - 1, LOGARG(ctx ? PARSER_CTX(ctx) : NULL, date_len), LY_EINVAL);

    /* check format */
    for (uint8_t i = 0; i < date_len; i++) {
        if (i == 4 || i == 7) {
            if (date[i] != '-') {
                goto error;
            }
        } else if (!isdigit(date[i])) {
            goto error;
        }
    }

    /* check content, e.g. 2018-02-31 */
    memset(&tm, 0, sizeof tm);
    r = strptime(date, "%Y-%m-%d", &tm);
    if (!r || r != &date[LY_REV_SIZE - 1]) {
        goto error;
    }
    memcpy(&tm_, &tm, sizeof tm);
    mktime(&tm_); /* mktime modifies tm_ if it refers invalid date */
    if (tm.tm_mday != tm_.tm_mday) { /* e.g 2018-02-29 -> 2018-03-01 */
        /* checking days is enough, since other errors
         * have been checked by strptime() */
        goto error;
    }

    return LY_SUCCESS;

error:
    if (stmt) {
        if (ctx) {
            LOGVAL_PARSER(ctx, LY_VCODE_INVAL, date_len, date, stmt);
        } else {
            LOGVAL(NULL, LY_VLOG_NONE, NULL, LY_VCODE_INVAL, date_len, date, stmt);
        }
    }
    return LY_EINVAL;
}

void
lysp_sort_revisions(struct lysp_revision *revs)
{
    LY_ARRAY_COUNT_TYPE i, r;
    struct lysp_revision rev;

    for (i = 1, r = 0; revs && i < LY_ARRAY_COUNT(revs); i++) {
        if (strcmp(revs[i].date, revs[r].date) > 0) {
            r = i;
        }
    }

    if (r) {
        /* the newest revision is not on position 0, switch them */
        memcpy(&rev, &revs[0], sizeof rev);
        memcpy(&revs[0], &revs[r], sizeof rev);
        memcpy(&revs[r], &rev, sizeof rev);
    }
}

static const struct lysp_tpdf *
lysp_type_match(const char *name, struct lysp_node *node)
{
    const struct lysp_tpdf *typedefs;
    LY_ARRAY_COUNT_TYPE u;

    typedefs = lysp_node_typedefs(node);
    LY_ARRAY_FOR(typedefs, u) {
        if (!strcmp(name, typedefs[u].name)) {
            /* match */
            return &typedefs[u];
        }
    }

    return NULL;
}

static LY_DATA_TYPE
lysp_type_str2builtin(const char *name, size_t len)
{
    if (len >= 4) { /* otherwise it does not match any built-in type */
        if (name[0] == 'b') {
            if (name[1] == 'i') {
                if (len == 6 && !strncmp(&name[2], "nary", 4)) {
                    return LY_TYPE_BINARY;
                } else if (len == 4 && !strncmp(&name[2], "ts", 2)) {
                    return LY_TYPE_BITS;
                }
            } else if (len == 7 && !strncmp(&name[1], "oolean", 6)) {
                return LY_TYPE_BOOL;
            }
        } else if (name[0] == 'd') {
            if (len == 9 && !strncmp(&name[1], "ecimal64", 8)) {
                return LY_TYPE_DEC64;
            }
        } else if (name[0] == 'e') {
            if (len == 5 && !strncmp(&name[1], "mpty", 4)) {
                return LY_TYPE_EMPTY;
            } else if (len == 11 && !strncmp(&name[1], "numeration", 10)) {
                return LY_TYPE_ENUM;
            }
        } else if (name[0] == 'i') {
            if (name[1] == 'n') {
                if (len == 4 && !strncmp(&name[2], "t8", 2)) {
                    return LY_TYPE_INT8;
                } else if (len == 5) {
                    if (!strncmp(&name[2], "t16", 3)) {
                        return LY_TYPE_INT16;
                    } else if (!strncmp(&name[2], "t32", 3)) {
                        return LY_TYPE_INT32;
                    } else if (!strncmp(&name[2], "t64", 3)) {
                        return LY_TYPE_INT64;
                    }
                } else if (len == 19 && !strncmp(&name[2], "stance-identifier", 17)) {
                    return LY_TYPE_INST;
                }
            } else if (len == 11 && !strncmp(&name[1], "dentityref", 10)) {
                return LY_TYPE_IDENT;
            }
        } else if (name[0] == 'l') {
            if (len == 7 && !strncmp(&name[1], "eafref", 6)) {
                return LY_TYPE_LEAFREF;
            }
        } else if (name[0] == 's') {
            if (len == 6 && !strncmp(&name[1], "tring", 5)) {
                return LY_TYPE_STRING;
            }
        } else if (name[0] == 'u') {
            if (name[1] == 'n') {
                if (len == 5 && !strncmp(&name[2], "ion", 3)) {
                    return LY_TYPE_UNION;
                }
            } else if (name[1] == 'i' && name[2] == 'n' && name[3] == 't') {
                if (len == 5 && name[4] == '8') {
                    return LY_TYPE_UINT8;
                } else if (len == 6) {
                    if (!strncmp(&name[4], "16", 2)) {
                        return LY_TYPE_UINT16;
                    } else if (!strncmp(&name[4], "32", 2)) {
                        return LY_TYPE_UINT32;
                    } else if (!strncmp(&name[4], "64", 2)) {
                        return LY_TYPE_UINT64;
                    }
                }
            }
        }
    }

    return LY_TYPE_UNKNOWN;
}

LY_ERR
lysp_type_find(const char *id, struct lysp_node *start_node, struct lysp_module *start_module,
        LY_DATA_TYPE *type, const struct lysp_tpdf **tpdf, struct lysp_node **node, struct lysp_module **module)
{
    const char *str, *name;
    struct lysp_tpdf *typedefs;
    struct lys_module *mod;
    LY_ARRAY_COUNT_TYPE u, v;

    assert(id);
    assert(start_module);
    assert(tpdf);
    assert(node);
    assert(module);

    *node = NULL;
    str = strchr(id, ':');
    if (str) {
        mod = lys_module_find_prefix(start_module->mod, id, str - id);
        *module = mod ? mod->parsed : NULL;
        name = str + 1;
        *type = LY_TYPE_UNKNOWN;
    } else {
        *module = start_module;
        name = id;

        /* check for built-in types */
        *type = lysp_type_str2builtin(name, strlen(name));
        if (*type) {
            *tpdf = NULL;
            return LY_SUCCESS;
        }
    }
    LY_CHECK_RET(!(*module), LY_ENOTFOUND);

    if (start_node && *module == start_module) {
        /* search typedefs in parent's nodes */
        *node = start_node;
        while (*node) {
            *tpdf = lysp_type_match(name, *node);
            if (*tpdf) {
                /* match */
                return LY_SUCCESS;
            }
            *node = (*node)->parent;
        }
    }

    /* search in top-level typedefs */
    if ((*module)->typedefs) {
        LY_ARRAY_FOR((*module)->typedefs, u) {
            if (!strcmp(name, (*module)->typedefs[u].name)) {
                /* match */
                *tpdf = &(*module)->typedefs[u];
                return LY_SUCCESS;
            }
        }
    }

    /* search in submodules' typedefs */
    LY_ARRAY_FOR((*module)->includes, u) {
        typedefs = (*module)->includes[u].submodule->typedefs;
        LY_ARRAY_FOR(typedefs, v) {
            if (!strcmp(name, typedefs[v].name)) {
                /* match */
                *tpdf = &typedefs[v];
                return LY_SUCCESS;
            }
        }
    }

    return LY_ENOTFOUND;
}

LY_ERR
lysp_check_enum_name(struct lys_parser_ctx *ctx, const char *name, size_t name_len)
{
    if (!name_len) {
        LOGVAL_PARSER(ctx, LYVE_SYNTAX_YANG, "Enum name must not be zero-length.");
        return LY_EVALID;
    } else if (isspace(name[0]) || isspace(name[name_len - 1])) {
        LOGVAL_PARSER(ctx, LYVE_SYNTAX_YANG, "Enum name must not have any leading or trailing whitespaces (\"%.*s\").",
                    name_len, name);
        return LY_EVALID;
    } else {
        for (size_t u = 0; u < name_len; ++u) {
            if (iscntrl(name[u])) {
                LOGWRN(PARSER_CTX(ctx), "Control characters in enum name should be avoided (\"%.*s\", character number %d).",
                    name_len, name, u + 1);
                break;
            }
        }
    }

    return LY_SUCCESS;
}

/**
 * @brief Check name of a new type to avoid name collisions.
 *
 * @param[in] ctx Parser context, module where the type is being defined is taken from here.
 * @param[in] node Schema node where the type is being defined, NULL in case of a top-level typedef.
 * @param[in] tpdf Typedef definition to check.
 * @param[in,out] tpdfs_global Initialized hash table to store temporary data between calls. When the module's
 *            typedefs are checked, caller is supposed to free the table.
 * @param[in,out] tpdfs_global Initialized hash table to store temporary data between calls. When the module's
 *            typedefs are checked, caller is supposed to free the table.
 * @return LY_EEXIST in case of collision, LY_SUCCESS otherwise.
 */
static LY_ERR
lysp_check_typedef(struct lys_parser_ctx *ctx, struct lysp_node *node, const struct lysp_tpdf *tpdf,
        struct hash_table *tpdfs_global, struct hash_table *tpdfs_scoped)
{
    struct lysp_node *parent;
    uint32_t hash;
    size_t name_len;
    const char *name;
    LY_ARRAY_COUNT_TYPE u;
    const struct lysp_tpdf *typedefs;

    assert(ctx);
    assert(tpdf);

    name = tpdf->name;
    name_len = strlen(name);

    if (lysp_type_str2builtin(name, name_len)) {
        LOGVAL_PARSER(ctx, LYVE_SYNTAX_YANG, "Invalid name \"%s\" of typedef - name collision with a built-in type.", name);
        return LY_EEXIST;
    }

    /* check locally scoped typedefs (avoid name shadowing) */
    if (node) {
        typedefs = lysp_node_typedefs(node);
        LY_ARRAY_FOR(typedefs, u) {
            if (&typedefs[u] == tpdf) {
                break;
            }
            if (!strcmp(name, typedefs[u].name)) {
                LOGVAL_PARSER(ctx, LYVE_SYNTAX_YANG, "Invalid name \"%s\" of typedef - name collision with sibling type.", name);
                return LY_EEXIST;
            }
        }
        /* search typedefs in parent's nodes */
        for (parent = node->parent; parent; parent = parent->parent) {
            if (lysp_type_match(name, parent)) {
                LOGVAL_PARSER(ctx, LYVE_SYNTAX_YANG, "Invalid name \"%s\" of typedef - name collision with another scoped type.", name);
                return LY_EEXIST;
            }
        }
    }

    /* check collision with the top-level typedefs */
    hash = dict_hash(name, name_len);
    if (node) {
        lyht_insert(tpdfs_scoped, &name, hash, NULL);
        if (!lyht_find(tpdfs_global, &name, hash, NULL)) {
            LOGVAL_PARSER(ctx, LYVE_SYNTAX_YANG, "Invalid name \"%s\" of typedef - scoped type collide with a top-level type.", name);
            return LY_EEXIST;
        }
    } else {
        if (lyht_insert(tpdfs_global, &name, hash, NULL)) {
            LOGVAL_PARSER(ctx, LYVE_SYNTAX_YANG, "Invalid name \"%s\" of typedef - name collision with another top-level type.", name);
            return LY_EEXIST;
        }
        /* it is not necessary to test collision with the scoped types - in lysp_check_typedefs, all the
         * top-level typedefs are inserted into the tables before the scoped typedefs, so the collision
         * is detected in the first branch few lines above */
    }

    return LY_SUCCESS;
}

/**
 * @brief Compare identifiers.
 * Implementation of ::values_equal_cb.
 */
static ly_bool
lysp_id_cmp(void *val1, void *val2, ly_bool UNUSED(mod), void *UNUSED(cb_data))
{
    return strcmp(val1, val2) == 0 ? 1 : 0;
}

LY_ERR
lysp_parse_finalize_reallocated(struct lys_parser_ctx *ctx, struct lysp_grp *groupings, struct lysp_augment *augments,
        struct lysp_action *actions, struct lysp_notif *notifs)
{
    LY_ARRAY_COUNT_TYPE u, v;
    struct lysp_node *child;

    /* finalize parent pointers to the reallocated items */

    /* gropings */
    LY_ARRAY_FOR(groupings, u) {
        LY_LIST_FOR(groupings[u].data, child) {
            child->parent = (struct lysp_node *)&groupings[u];
        }
        LY_ARRAY_FOR(groupings[u].actions, v) {
            groupings[u].actions[v].parent = (struct lysp_node *)&groupings[u];
        }
        LY_ARRAY_FOR(groupings[u].notifs, v) {
            groupings[u].notifs[v].parent = (struct lysp_node *)&groupings[u];
        }
        LY_ARRAY_FOR(groupings[u].groupings, v) {
            groupings[u].groupings[v].parent = (struct lysp_node *)&groupings[u];
        }
        if (groupings[u].typedefs) {
            LY_CHECK_RET(ly_set_add(&ctx->tpdfs_nodes, &groupings[u], 0, NULL));
        }
    }

    /* augments */
    LY_ARRAY_FOR(augments, u) {
        LY_LIST_FOR(augments[u].child, child) {
            child->parent = (struct lysp_node *)&augments[u];
        }
        LY_ARRAY_FOR(augments[u].actions, v) {
            augments[u].actions[v].parent = (struct lysp_node *)&augments[u];
        }
        LY_ARRAY_FOR(augments[u].notifs, v) {
            augments[u].notifs[v].parent = (struct lysp_node *)&augments[u];
        }
    }

    /* actions */
    LY_ARRAY_FOR(actions, u) {
        if (actions[u].input.parent) {
            actions[u].input.parent = (struct lysp_node *)&actions[u];
            LY_LIST_FOR(actions[u].input.data, child) {
                child->parent = (struct lysp_node *)&actions[u].input;
            }
            LY_ARRAY_FOR(actions[u].input.groupings, v) {
                actions[u].input.groupings[v].parent = (struct lysp_node *)&actions[u].input;
            }
            if (actions[u].input.typedefs) {
                LY_CHECK_RET(ly_set_add(&ctx->tpdfs_nodes, &actions[u].input, 0, NULL));
            }
        }
        if (actions[u].output.parent) {
            actions[u].output.parent = (struct lysp_node *)&actions[u];
            LY_LIST_FOR(actions[u].output.data, child) {
                child->parent = (struct lysp_node *)&actions[u].output;
            }
            LY_ARRAY_FOR(actions[u].output.groupings, v) {
                actions[u].output.groupings[v].parent = (struct lysp_node *)&actions[u].output;
            }
            if (actions[u].output.typedefs) {
                LY_CHECK_RET(ly_set_add(&ctx->tpdfs_nodes, &actions[u].output, 0, NULL));
            }
        }
        LY_ARRAY_FOR(actions[u].groupings, v) {
            actions[u].groupings[v].parent = (struct lysp_node *)&actions[u];
        }
        if (actions[u].typedefs) {
            LY_CHECK_RET(ly_set_add(&ctx->tpdfs_nodes, &actions[u], 0, NULL));
        }
    }

    /* notifications */
    LY_ARRAY_FOR(notifs, u) {
        LY_LIST_FOR(notifs[u].data, child) {
            child->parent = (struct lysp_node *)&notifs[u];
        }
        LY_ARRAY_FOR(notifs[u].groupings, v) {
            notifs[u].groupings[v].parent = (struct lysp_node *)&notifs[u];
        }
        if (notifs[u].typedefs) {
            LY_CHECK_RET(ly_set_add(&ctx->tpdfs_nodes, &notifs[u], 0, NULL));
        }
    }

    return LY_SUCCESS;
}

LY_ERR
lysp_check_typedefs(struct lys_parser_ctx *ctx, struct lysp_module *mod)
{
    struct hash_table *ids_global;
    struct hash_table *ids_scoped;
    const struct lysp_tpdf *typedefs;
    LY_ARRAY_COUNT_TYPE u, v;
    uint32_t i;
    LY_ERR ret = LY_EVALID;

    /* check name collisions - typedefs and groupings */
    ids_global = lyht_new(8, sizeof(char *), lysp_id_cmp, NULL, 1);
    ids_scoped = lyht_new(8, sizeof(char *), lysp_id_cmp, NULL, 1);
    LY_ARRAY_FOR(mod->typedefs, v) {
        if (lysp_check_typedef(ctx, NULL, &mod->typedefs[v], ids_global, ids_scoped)) {
            goto cleanup;
        }
    }
    LY_ARRAY_FOR(mod->includes, v) {
        LY_ARRAY_FOR(mod->includes[v].submodule->typedefs, u) {
            if (lysp_check_typedef(ctx, NULL, &mod->includes[v].submodule->typedefs[u], ids_global, ids_scoped)) {
                goto cleanup;
            }
        }
    }
    for (i = 0; i < ctx->tpdfs_nodes.count; ++i) {
        typedefs = lysp_node_typedefs((struct lysp_node *)ctx->tpdfs_nodes.objs[i]);
        LY_ARRAY_FOR(typedefs, u) {
            if (lysp_check_typedef(ctx, (struct lysp_node *)ctx->tpdfs_nodes.objs[i], &typedefs[u], ids_global, ids_scoped)) {
                goto cleanup;
            }
        }
    }
    ret = LY_SUCCESS;
cleanup:
    lyht_free(ids_global);
    lyht_free(ids_scoped);
    ly_set_erase(&ctx->tpdfs_nodes, NULL);

    return ret;
}

struct lysp_load_module_check_data {
    const char *name;
    const char *revision;
    const char *path;
    const char *submoduleof;
};

static LY_ERR
lysp_load_module_check(const struct ly_ctx *ctx, struct lysp_module *mod, struct lysp_submodule *submod, void *data)
{
    struct lysp_load_module_check_data *info = data;
    const char *filename, *dot, *rev, *name;
    uint8_t latest_revision;
    size_t len;
    struct lysp_revision *revs;

    name = mod ? mod->mod->name : submod->name;
    revs = mod ? mod->revs : submod->revs;
    latest_revision = mod ? mod->mod->latest_revision : submod->latest_revision;

    if (info->name) {
        /* check name of the parsed model */
        if (strcmp(info->name, name)) {
            LOGERR(ctx, LY_EINVAL, "Unexpected module \"%s\" parsed instead of \"%s\").", name, info->name);
            return LY_EINVAL;
        }
    }
    if (info->revision) {
        /* check revision of the parsed model */
        if (!revs || strcmp(info->revision, revs[0].date)) {
            LOGERR(ctx, LY_EINVAL, "Module \"%s\" parsed with the wrong revision (\"%s\" instead \"%s\").", name,
                   revs ? revs[0].date : "none", info->revision);
            return LY_EINVAL;
        }
    } else if (!latest_revision) {
        /* do not log, we just need to drop the schema and use the latest revision from the context */
        return LY_EEXIST;
    }
    if (submod) {
        assert(info->submoduleof);

        /* check that the submodule belongs-to our module */
        if (strcmp(info->submoduleof, submod->mod->name)) {
            LOGVAL(ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE, "Included \"%s\" submodule from \"%s\" belongs-to a different module \"%s\".",
                   submod->name, info->submoduleof, submod->mod->name);
            return LY_EVALID;
        }
        /* check circular dependency */
        if (submod->parsing) {
            LOGVAL(ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE, "A circular dependency (include) for module \"%s\".", submod->name);
            return LY_EVALID;
        }
    }
    if (info->path) {
        /* check that name and revision match filename */
        filename = strrchr(info->path, '/');
        if (!filename) {
            filename = info->path;
        } else {
            filename++;
        }
        /* name */
        len = strlen(name);
        rev = strchr(filename, '@');
        dot = strrchr(info->path, '.');
        if (strncmp(filename, name, len) ||
                ((rev && rev != &filename[len]) || (!rev && dot != &filename[len]))) {
            LOGWRN(ctx, "File name \"%s\" does not match module name \"%s\".", filename, name);
        }
        /* revision */
        if (rev) {
            len = dot - ++rev;
            if (!revs || len != 10 || strncmp(revs[0].date, rev, len)) {
                LOGWRN(ctx, "File name \"%s\" does not match module revision \"%s\".", filename,
                       revs ? revs[0].date : "none");
            }
        }
    }
    return LY_SUCCESS;
}

LY_ERR
lys_module_localfile(struct ly_ctx *ctx, const char *name, const char *revision, ly_bool implement,
        struct lys_parser_ctx *main_ctx, const char *main_name, ly_bool required, void **result)
{
    struct ly_in *in;
    char *filepath = NULL;
    LYS_INFORMAT format;
    void *mod = NULL;
    LY_ERR ret = LY_SUCCESS;
    struct lysp_load_module_check_data check_data = {0};

    LY_CHECK_RET(lys_search_localfile(ly_ctx_get_searchdirs(ctx), !(ctx->flags & LY_CTX_DISABLE_SEARCHDIR_CWD), name, revision,
                                      &filepath, &format));
    if (!filepath) {
        if (required) {
            LOGERR(ctx, LY_ENOTFOUND, "Data model \"%s%s%s\" not found in local searchdirs.", name, revision ? "@" : "",
                   revision ? revision : "");
        }
        return LY_ENOTFOUND;
    }

    LOGVRB("Loading schema from \"%s\" file.", filepath);

    /* get the (sub)module */
    LY_CHECK_ERR_GOTO(ret = ly_in_new_filepath(filepath, 0, &in),
                      LOGERR(ctx, ret, "Unable to create input handler for filepath %s.", filepath), cleanup);
    check_data.name = name;
    check_data.revision = revision;
    check_data.path = filepath;
    check_data.submoduleof = main_name;
    if (main_ctx) {
        ret = lys_parse_submodule(ctx, in, format, main_ctx, lysp_load_module_check, &check_data,
                        (struct lysp_submodule **)&mod);
    } else {
        ret = lys_create_module(ctx, in, format, implement, lysp_load_module_check, &check_data,
                        (struct lys_module **)&mod);

    }
    ly_in_free(in, 1);
    LY_CHECK_GOTO(ret, cleanup);

    *result = mod;

    /* success */

cleanup:
    free(filepath);
    return ret;
}

LY_ERR
lysp_load_module(struct ly_ctx *ctx, const char *name, const char *revision, ly_bool implement, ly_bool require_parsed,
        struct lys_module **mod)
{
    const char *module_data = NULL;
    LYS_INFORMAT format = LYS_IN_UNKNOWN;
    void (*module_data_free)(void *module_data, void *user_data) = NULL;
    struct lysp_load_module_check_data check_data = {0};
    struct lys_module *m = NULL;
    struct ly_in *in;

    assert(mod);

    if (ctx->flags & LY_CTX_ALLIMPLEMENTED) {
        implement = 1;
    }

    if (!*mod) {
        /* try to get the module from the context */
        if (revision) {
            /* get the specific revision */
            *mod = (struct lys_module *)ly_ctx_get_module(ctx, name, revision);
        } else if (implement) {
            /* prefer the implemented module instead of the latest one */
            *mod = (struct lys_module *)ly_ctx_get_module_implemented(ctx, name);
            if (!*mod) {
                /* there is no implemented module in the context, try to get the latest revision module */
                goto latest_in_the_context;
            }
        } else {
            /* get the requested module of the latest revision in the context */
latest_in_the_context:
            *mod = (struct lys_module *)ly_ctx_get_module_latest(ctx, name);
            if (*mod && (*mod)->latest_revision == 1) {
                /* let us now search with callback and searchpaths to check if there is newer revision outside the context */
                m = *mod;
                *mod = NULL;
            }
        }
    }

    if (!(*mod) || (require_parsed && !(*mod)->parsed)) {
        (*mod) = NULL;

        /* check collision with other implemented revision */
        if (implement && ly_ctx_get_module_implemented(ctx, name)) {
            LOGVAL(ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE,
                   "Module \"%s\" is already present in other implemented revision.", name);
            return LY_EDENIED;
        }

        /* module not present in the context, get the input data and parse it */
        if (!(ctx->flags & LY_CTX_PREFER_SEARCHDIRS)) {
search_clb:
            if (ctx->imp_clb) {
                if (ctx->imp_clb(name, revision, NULL, NULL, ctx->imp_clb_data,
                                      &format, &module_data, &module_data_free) == LY_SUCCESS) {
                    LY_CHECK_RET(ly_in_new_memory(module_data, &in));
                    check_data.name = name;
                    check_data.revision = revision;
                    lys_create_module(ctx, in, format, implement, lysp_load_module_check, &check_data, mod);
                    ly_in_free(in, 0);
                    if (module_data_free) {
                        module_data_free((void *)module_data, ctx->imp_clb_data);
                    }
                }
            }
            if (!(*mod) && !(ctx->flags & LY_CTX_PREFER_SEARCHDIRS)) {
                goto search_file;
            }
        } else {
search_file:
            if (!(ctx->flags & LY_CTX_DISABLE_SEARCHDIRS)) {
                /* module was not received from the callback or there is no callback set */
                lys_module_localfile(ctx, name, revision, implement, NULL, NULL, m ? 0 : 1, (void **)mod);
            }
            if (!(*mod) && (ctx->flags & LY_CTX_PREFER_SEARCHDIRS)) {
                goto search_clb;
            }
        }

        /* update the latest_revision flag - here we have selected the latest available schema,
         * consider that even the callback provides correct latest revision */
        if (!(*mod) && m) {
            LOGVRB("Newer revision than %s-%s not found, using this as the latest revision.", m->name, m->revision);
            m->latest_revision = 2;
            *mod = m;
        } else if ((*mod) && !revision && ((*mod)->latest_revision == 1)) {
            (*mod)->latest_revision = 2;
        }
    } else {
        /* we have module from the current context */
        if (implement) {
            m = ly_ctx_get_module_implemented(ctx, name);
            if (m && m != *mod) {
                /* check collision with other implemented revision */
                LOGVAL(ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE,
                       "Module \"%s\" is already present in other implemented revision.", name);
                *mod = NULL;
                return LY_EDENIED;
            }
        }

        /* circular check */
        if ((*mod)->parsed && (*mod)->parsed->parsing) {
            LOGVAL(ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE, "A circular dependency (import) for module \"%s\".", name);
            *mod = NULL;
            return LY_EVALID;
        }
    }
    if (!(*mod)) {
        LOGVAL(ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE, "%s \"%s\" module failed.", implement ? "Loading" : "Importing", name);
        return LY_EVALID;
    }

    if (implement) {
        /* mark the module implemented, check for collision was already done */
        (*mod)->implemented = 1;
    }

    return LY_SUCCESS;
}

LY_ERR
lysp_check_stringchar(struct lys_parser_ctx *ctx, uint32_t c)
{
    if (!is_yangutf8char(c)) {
        LOGVAL_PARSER(ctx, LY_VCODE_INCHAR, c);
        return LY_EVALID;
    }
    return LY_SUCCESS;
}

LY_ERR
lysp_check_identifierchar(struct lys_parser_ctx *ctx, uint32_t c, ly_bool first, uint8_t *prefix)
{
    if (first || (prefix && (*prefix) == 1)) {
        if (!is_yangidentstartchar(c)) {
            LOGVAL_PARSER(ctx, LYVE_SYNTAX_YANG, "Invalid identifier first character '%c' (0x%04x).", (char)c, c);
            return LY_EVALID;
        }
        if (prefix) {
            if (first) {
                (*prefix) = 0;
            } else {
                (*prefix) = 2;
            }
        }
    } else if (c == ':' && prefix && (*prefix) == 0) {
        (*prefix) = 1;
    } else if (!is_yangidentchar(c)) {
        LOGVAL_PARSER(ctx, LYVE_SYNTAX_YANG, "Invalid identifier character '%c' (0x%04x).", (char)c, c);
        return LY_EVALID;
    }

    return LY_SUCCESS;
}

LY_ERR
lysp_load_submodule(struct lys_parser_ctx *pctx, struct lysp_include *inc)
{
    struct ly_ctx *ctx = (struct ly_ctx *)(PARSER_CTX(pctx));
    struct lysp_submodule *submod = NULL;
    const char *submodule_data = NULL;
    LYS_INFORMAT format = LYS_IN_UNKNOWN;
    void (*submodule_data_free)(void *module_data, void *user_data) = NULL;
    struct lysp_load_module_check_data check_data = {0};
    struct ly_in *in;

    /* submodule not present in the context, get the input data and parse it */
    if (!(ctx->flags & LY_CTX_PREFER_SEARCHDIRS)) {
search_clb:
        if (ctx->imp_clb) {
            if (ctx->imp_clb(pctx->main_mod->name, NULL, inc->name, inc->rev[0] ? inc->rev : NULL, ctx->imp_clb_data,
                                  &format, &submodule_data, &submodule_data_free) == LY_SUCCESS) {
                LY_CHECK_RET(ly_in_new_memory(submodule_data, &in));
                check_data.name = inc->name;
                check_data.revision = inc->rev[0] ? inc->rev : NULL;
                check_data.submoduleof = pctx->main_mod->name;
                lys_parse_submodule(ctx, in, format, pctx, lysp_load_module_check, &check_data, &submod);
                ly_in_free(in, 0);
                if (submodule_data_free) {
                    submodule_data_free((void *)submodule_data, ctx->imp_clb_data);
                }
            }
        }
        if (!submod && !(ctx->flags & LY_CTX_PREFER_SEARCHDIRS)) {
            goto search_file;
        }
    } else {
search_file:
        if (!(ctx->flags & LY_CTX_DISABLE_SEARCHDIRS)) {
            /* submodule was not received from the callback or there is no callback set */
            lys_module_localfile(ctx, inc->name, inc->rev[0] ? inc->rev : NULL, 0, pctx, pctx->main_mod->name, 1,
                    (void **)&submod);
        }
        if (!submod && (ctx->flags & LY_CTX_PREFER_SEARCHDIRS)) {
            goto search_clb;
        }
    }
    if (submod) {
        if (!inc->rev[0] && (submod->latest_revision == 1)) {
            /* update the latest_revision flag - here we have selected the latest available schema,
             * consider that even the callback provides correct latest revision */
            submod->latest_revision = 2;
        }

        inc->submodule = submod;
    }
    if (!inc->submodule) {
        LOGVAL(ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE, "Including \"%s\" submodule into \"%s\" failed.",
               inc->name, pctx->main_mod->name);
        return LY_EVALID;
    }

    return LY_SUCCESS;
}

struct lys_module *
lys_module_find_prefix(const struct lys_module *mod, const char *prefix, size_t len)
{
    struct lys_module *m = NULL;
    LY_ARRAY_COUNT_TYPE u;

    if (!len || !ly_strncmp(mod->prefix, prefix, len)) {
        /* it is the prefix of the module itself */
        m = (struct lys_module *)mod;
    }

    /* search in imports */
    if (!m && mod->parsed) {
        LY_ARRAY_FOR(mod->parsed->imports, u) {
            if (!ly_strncmp(mod->parsed->imports[u].prefix, prefix, len)) {
                m = mod->parsed->imports[u].module;
                break;
            }
        }
    }

    return m;
}

const char *
lys_prefix_find_module(const struct lys_module *mod, const struct lys_module *import)
{
    LY_ARRAY_COUNT_TYPE u;

    if (import == mod) {
        return mod->prefix;
    }

    if (mod->parsed) {
        LY_ARRAY_FOR(mod->parsed->imports, u) {
            if (mod->parsed->imports[u].module == import) {
                return mod->parsed->imports[u].prefix;
            }
        }
    } else {
        /* we don't have original information about the import's prefix,
         * so the prefix of the import module itself is returned instead */
        return import->prefix;
    }

    return NULL;
}

API const char *
lys_nodetype2str(uint16_t nodetype)
{
    switch (nodetype) {
    case LYS_CONTAINER:
        return "container";
    case LYS_CHOICE:
        return "choice";
    case LYS_LEAF:
        return "leaf";
    case LYS_LEAFLIST:
        return "leaf-list";
    case LYS_LIST:
        return "list";
    case LYS_ANYXML:
        return "anyxml";
    case LYS_ANYDATA:
        return "anydata";
    case LYS_CASE:
        return "case";
    case LYS_RPC:
        return "RPC";
    case LYS_ACTION:
        return "action";
    case LYS_NOTIF:
        return "notification";
    case LYS_USES:
        return "uses";
    default:
        return "unknown";
    }
}

const char *
lys_datatype2str(LY_DATA_TYPE basetype)
{
    switch (basetype) {
    case LY_TYPE_BINARY:
        return "binary";
    case LY_TYPE_UINT8:
        return "uint8";
    case LY_TYPE_UINT16:
        return "uint16";
    case LY_TYPE_UINT32:
        return "uint32";
    case LY_TYPE_UINT64:
        return "uint64";
    case LY_TYPE_STRING:
        return "string";
    case LY_TYPE_BITS:
        return "bits";
    case LY_TYPE_BOOL:
        return "boolean";
    case LY_TYPE_DEC64:
        return "decimal64";
    case LY_TYPE_EMPTY:
        return "empty";
    case LY_TYPE_ENUM:
        return "enumeration";
    case LY_TYPE_IDENT:
        return "identityref";
    case LY_TYPE_INST:
        return "instance-identifier";
    case LY_TYPE_LEAFREF:
        return "leafref";
    case LY_TYPE_UNION:
        return "union";
    case LY_TYPE_INT8:
        return "int8";
    case LY_TYPE_INT16:
        return "int16";
    case LY_TYPE_INT32:
        return "int32";
    case LY_TYPE_INT64:
        return "int64";
    default:
        return "unknown";
    }
}

API const struct lysp_tpdf *
lysp_node_typedefs(const struct lysp_node *node)
{
    switch (node->nodetype) {
    case LYS_CONTAINER:
        return ((struct lysp_node_container *)node)->typedefs;
    case LYS_LIST:
        return ((struct lysp_node_list *)node)->typedefs;
    case LYS_GROUPING:
        return ((struct lysp_grp *)node)->typedefs;
    case LYS_RPC:
    case LYS_ACTION:
        return ((struct lysp_action *)node)->typedefs;
    case LYS_INPUT:
    case LYS_OUTPUT:
        return ((struct lysp_action_inout *)node)->typedefs;
    case LYS_NOTIF:
        return ((struct lysp_notif *)node)->typedefs;
    default:
        return NULL;
    }
}

API const struct lysp_grp *
lysp_node_groupings(const struct lysp_node *node)
{
    switch (node->nodetype) {
    case LYS_CONTAINER:
        return ((struct lysp_node_container *)node)->groupings;
    case LYS_LIST:
        return ((struct lysp_node_list *)node)->groupings;
    case LYS_GROUPING:
        return ((struct lysp_grp *)node)->groupings;
    case LYS_RPC:
    case LYS_ACTION:
        return ((struct lysp_action *)node)->groupings;
    case LYS_INPUT:
    case LYS_OUTPUT:
        return ((struct lysp_action_inout *)node)->groupings;
    case LYS_NOTIF:
        return ((struct lysp_notif *)node)->groupings;
    default:
        return NULL;
    }
}

struct lysp_action **
lysp_node_actions_p(struct lysp_node *node)
{
    assert(node);

    switch (node->nodetype) {
    case LYS_CONTAINER:
        return &((struct lysp_node_container *)node)->actions;
    case LYS_LIST:
        return &((struct lysp_node_list *)node)->actions;
    case LYS_GROUPING:
        return &((struct lysp_grp *)node)->actions;
    case LYS_AUGMENT:
        return &((struct lysp_augment *)node)->actions;
    default:
        return NULL;
    }
}

API const struct lysp_action *
lysp_node_actions(const struct lysp_node *node)
{
    struct lysp_action **actions;
    actions = lysp_node_actions_p((struct lysp_node *)node);
    if (actions) {
        return *actions;
    } else {
        return NULL;
    }
}

struct lysp_notif **
lysp_node_notifs_p(struct lysp_node *node)
{
    assert(node);
    switch (node->nodetype) {
    case LYS_CONTAINER:
        return &((struct lysp_node_container *)node)->notifs;
    case LYS_LIST:
        return &((struct lysp_node_list *)node)->notifs;
    case LYS_GROUPING:
        return &((struct lysp_grp *)node)->notifs;
    case LYS_AUGMENT:
        return &((struct lysp_augment *)node)->notifs;
    default:
        return NULL;
    }
}

API const struct lysp_notif *
lysp_node_notifs(const struct lysp_node *node)
{
    struct lysp_notif **notifs;
    notifs = lysp_node_notifs_p((struct lysp_node *)node);
    if (notifs) {
        return *notifs;
    } else {
        return NULL;
    }
}

struct lysp_node **
lysp_node_children_p(struct lysp_node *node)
{
    assert(node);
    switch (node->nodetype) {
    case LYS_CONTAINER:
        return &((struct lysp_node_container *)node)->child;
    case LYS_CHOICE:
        return &((struct lysp_node_choice *)node)->child;
    case LYS_LIST:
        return &((struct lysp_node_list *)node)->child;
    case LYS_CASE:
        return &((struct lysp_node_case *)node)->child;
    case LYS_GROUPING:
        return &((struct lysp_grp *)node)->data;
    case LYS_AUGMENT:
        return &((struct lysp_augment *)node)->child;
    case LYS_INPUT:
    case LYS_OUTPUT:
        return &((struct lysp_action_inout *)node)->data;
    case LYS_NOTIF:
        return &((struct lysp_notif *)node)->data;
    default:
        return NULL;
    }
}

API const struct lysp_node *
lysp_node_children(const struct lysp_node *node)
{
    struct lysp_node **children;

    if (!node) {
        return NULL;
    }

    children = lysp_node_children_p((struct lysp_node *)node);
    if (children) {
        return *children;
    } else {
        return NULL;
    }
}

struct lysc_action **
lysc_node_actions_p(struct lysc_node *node)
{
    assert(node);
    switch (node->nodetype) {
    case LYS_CONTAINER:
        return &((struct lysc_node_container *)node)->actions;
    case LYS_LIST:
        return &((struct lysc_node_list *)node)->actions;
    default:
        return NULL;
    }
}

API const struct lysc_action *
lysc_node_actions(const struct lysc_node *node)
{
    struct lysc_action **actions;
    actions = lysc_node_actions_p((struct lysc_node *)node);
    if (actions) {
        return *actions;
    } else {
        return NULL;
    }
}

struct lysc_notif **
lysc_node_notifs_p(struct lysc_node *node)
{
    assert(node);
    switch (node->nodetype) {
    case LYS_CONTAINER:
        return &((struct lysc_node_container *)node)->notifs;
    case LYS_LIST:
        return &((struct lysc_node_list *)node)->notifs;
    default:
        return NULL;
    }
}

API const struct lysc_notif *
lysc_node_notifs(const struct lysc_node *node)
{
    struct lysc_notif **notifs;
    notifs = lysc_node_notifs_p((struct lysc_node *)node);
    if (notifs) {
        return *notifs;
    } else {
        return NULL;
    }
}

struct lysc_node **
lysc_node_children_p(const struct lysc_node *node, uint16_t flags)
{
    assert(node);
    switch (node->nodetype) {
    case LYS_CONTAINER:
        return &((struct lysc_node_container *)node)->child;
    case LYS_CHOICE:
        return (struct lysc_node **)&((struct lysc_node_choice *)node)->cases;
    case LYS_CASE:
        return &((struct lysc_node_case *)node)->child;
    case LYS_LIST:
        return &((struct lysc_node_list *)node)->child;
    case LYS_RPC:
    case LYS_ACTION:
        if (flags & LYS_CONFIG_R) {
            return &((struct lysc_action *)node)->output.data;
        } else {
            /* LYS_CONFIG_W, but also the default case */
            return &((struct lysc_action *)node)->input.data;
        }
    case LYS_INPUT:
    case LYS_OUTPUT:
        return &((struct lysc_action_inout *)node)->data;
    case LYS_NOTIF:
        return &((struct lysc_notif *)node)->data;
    default:
        return NULL;
    }
}

API const struct lysc_node *
lysc_node_children(const struct lysc_node *node, uint16_t flags)
{
    struct lysc_node **children;

    if (!node) {
        return NULL;
    }

    children = lysc_node_children_p((struct lysc_node *)node, flags);
    if (children) {
        return *children;
    } else {
        return NULL;
    }
}

struct lys_module *
lysp_find_module(struct ly_ctx *ctx, const struct lysp_module *mod)
{
    for (uint32_t u = 0; u < ctx->list.count; ++u) {
        if (((struct lys_module *)ctx->list.objs[u])->parsed == mod) {
            return ((struct lys_module *)ctx->list.objs[u]);
        }
    }
    return NULL;
}

enum ly_stmt
lysp_match_kw(struct lys_yang_parser_ctx *ctx, struct ly_in *in)
{
/**
 * @brief Move the INPUT by COUNT items. Also updates the indent value in yang parser context
 * @param[in] CTX yang parser context to update its indent value.
 * @param[in,out] IN input to move
 * @param[in] COUNT number of items for which the DATA pointer is supposed to move on.
 *
 * *INDENT-OFF*
 */
#define MOVE_IN(CTX, IN, COUNT) ly_in_skip(IN, COUNT);if(CTX){(CTX)->indent+=COUNT;}
#define IF_KW(STR, LEN, STMT) if (!strncmp(in->current, STR, LEN)) {MOVE_IN(ctx, in, LEN);*kw=STMT;}
#define IF_KW_PREFIX(STR, LEN) if (!strncmp(in->current, STR, LEN)) {MOVE_IN(ctx, in, LEN);
#define IF_KW_PREFIX_END }

    const char *start = in->current;
    enum ly_stmt result = LY_STMT_NONE;
    enum ly_stmt *kw = &result;
    /* read the keyword itself */
    switch (in->current[0]) {
    case 'a':
        MOVE_IN(ctx, in, 1);
        IF_KW("rgument", 7, LY_STMT_ARGUMENT)
        else IF_KW("ugment", 6, LY_STMT_AUGMENT)
        else IF_KW("ction", 5, LY_STMT_ACTION)
        else IF_KW_PREFIX("ny", 2)
            IF_KW("data", 4, LY_STMT_ANYDATA)
            else IF_KW("xml", 3, LY_STMT_ANYXML)
        IF_KW_PREFIX_END
        break;
    case 'b':
        MOVE_IN(ctx, in, 1);
        IF_KW("ase", 3, LY_STMT_BASE)
        else IF_KW("elongs-to", 9, LY_STMT_BELONGS_TO)
        else IF_KW("it", 2, LY_STMT_BIT)
        break;
    case 'c':
        MOVE_IN(ctx, in, 1);
        IF_KW("ase", 3, LY_STMT_CASE)
        else IF_KW("hoice", 5, LY_STMT_CHOICE)
        else IF_KW_PREFIX("on", 2)
            IF_KW("fig", 3, LY_STMT_CONFIG)
            else IF_KW_PREFIX("ta", 2)
                IF_KW("ct", 2, LY_STMT_CONTACT)
                else IF_KW("iner", 4, LY_STMT_CONTAINER)
            IF_KW_PREFIX_END
        IF_KW_PREFIX_END
        break;
    case 'd':
        MOVE_IN(ctx, in, 1);
        IF_KW_PREFIX("e", 1)
            IF_KW("fault", 5, LY_STMT_DEFAULT)
            else IF_KW("scription", 9, LY_STMT_DESCRIPTION)
            else IF_KW_PREFIX("viat", 4)
                IF_KW("e", 1, LY_STMT_DEVIATE)
                else IF_KW("ion", 3, LY_STMT_DEVIATION)
            IF_KW_PREFIX_END
        IF_KW_PREFIX_END
        break;
    case 'e':
        MOVE_IN(ctx, in, 1);
        IF_KW("num", 3, LY_STMT_ENUM)
        else IF_KW_PREFIX("rror-", 5)
            IF_KW("app-tag", 7, LY_STMT_ERROR_APP_TAG)
            else IF_KW("message", 7, LY_STMT_ERROR_MESSAGE)
        IF_KW_PREFIX_END
        else IF_KW("xtension", 8, LY_STMT_EXTENSION)
        break;
    case 'f':
        MOVE_IN(ctx, in, 1);
        IF_KW("eature", 6, LY_STMT_FEATURE)
        else IF_KW("raction-digits", 14, LY_STMT_FRACTION_DIGITS)
        break;
    case 'g':
        MOVE_IN(ctx, in, 1);
        IF_KW("rouping", 7, LY_STMT_GROUPING)
        break;
    case 'i':
        MOVE_IN(ctx, in, 1);
        IF_KW("dentity", 7, LY_STMT_IDENTITY)
        else IF_KW("f-feature", 9, LY_STMT_IF_FEATURE)
        else IF_KW("mport", 5, LY_STMT_IMPORT)
        else IF_KW_PREFIX("n", 1)
            IF_KW("clude", 5, LY_STMT_INCLUDE)
            else IF_KW("put", 3, LY_STMT_INPUT)
        IF_KW_PREFIX_END
        break;
    case 'k':
        MOVE_IN(ctx, in, 1);
        IF_KW("ey", 2, LY_STMT_KEY)
        break;
    case 'l':
        MOVE_IN(ctx, in, 1);
        IF_KW_PREFIX("e", 1)
            IF_KW("af-list", 7, LY_STMT_LEAF_LIST)
            else IF_KW("af", 2, LY_STMT_LEAF)
            else IF_KW("ngth", 4, LY_STMT_LENGTH)
        IF_KW_PREFIX_END
        else IF_KW("ist", 3, LY_STMT_LIST)
        break;
    case 'm':
        MOVE_IN(ctx, in, 1);
        IF_KW_PREFIX("a", 1)
            IF_KW("ndatory", 7, LY_STMT_MANDATORY)
            else IF_KW("x-elements", 10, LY_STMT_MAX_ELEMENTS)
        IF_KW_PREFIX_END
        else IF_KW("in-elements", 11, LY_STMT_MIN_ELEMENTS)
        else IF_KW("ust", 3, LY_STMT_MUST)
        else IF_KW_PREFIX("od", 2)
            IF_KW("ule", 3, LY_STMT_MODULE)
            else IF_KW("ifier", 5, LY_STMT_MODIFIER)
        IF_KW_PREFIX_END
        break;
    case 'n':
        MOVE_IN(ctx, in, 1);
        IF_KW("amespace", 8, LY_STMT_NAMESPACE)
        else IF_KW("otification", 11, LY_STMT_NOTIFICATION)
        break;
    case 'o':
        MOVE_IN(ctx, in, 1);
        IF_KW_PREFIX("r", 1)
            IF_KW("dered-by", 8, LY_STMT_ORDERED_BY)
            else IF_KW("ganization", 10, LY_STMT_ORGANIZATION)
        IF_KW_PREFIX_END
        else IF_KW("utput", 5, LY_STMT_OUTPUT)
        break;
    case 'p':
        MOVE_IN(ctx, in, 1);
        IF_KW("ath", 3, LY_STMT_PATH)
        else IF_KW("attern", 6, LY_STMT_PATTERN)
        else IF_KW("osition", 7, LY_STMT_POSITION)
        else IF_KW_PREFIX("re", 2)
            IF_KW("fix", 3, LY_STMT_PREFIX)
            else IF_KW("sence", 5, LY_STMT_PRESENCE)
        IF_KW_PREFIX_END
        break;
    case 'r':
        MOVE_IN(ctx, in, 1);
        IF_KW("ange", 4, LY_STMT_RANGE)
        else IF_KW_PREFIX("e", 1)
            IF_KW_PREFIX("f", 1)
                IF_KW("erence", 6, LY_STMT_REFERENCE)
                else IF_KW("ine", 3, LY_STMT_REFINE)
            IF_KW_PREFIX_END
            else IF_KW("quire-instance", 14, LY_STMT_REQUIRE_INSTANCE)
            else IF_KW("vision-date", 11, LY_STMT_REVISION_DATE)
            else IF_KW("vision", 6, LY_STMT_REVISION)
        IF_KW_PREFIX_END
        else IF_KW("pc", 2, LY_STMT_RPC)
        break;
    case 's':
        MOVE_IN(ctx, in, 1);
        IF_KW("tatus", 5, LY_STMT_STATUS)
        else IF_KW("ubmodule", 8, LY_STMT_SUBMODULE)
        break;
    case 't':
        MOVE_IN(ctx, in, 1);
        IF_KW("ypedef", 6, LY_STMT_TYPEDEF)
        else IF_KW("ype", 3, LY_STMT_TYPE)
        break;
    case 'u':
        MOVE_IN(ctx, in, 1);
        IF_KW_PREFIX("ni", 2)
            IF_KW("que", 3, LY_STMT_UNIQUE)
            else IF_KW("ts", 2, LY_STMT_UNITS)
        IF_KW_PREFIX_END
        else IF_KW("ses", 3, LY_STMT_USES)
        break;
    case 'v':
        MOVE_IN(ctx, in, 1);
        IF_KW("alue", 4, LY_STMT_VALUE)
        break;
    case 'w':
        MOVE_IN(ctx, in, 1);
        IF_KW("hen", 3, LY_STMT_WHEN)
        break;
    case 'y':
        MOVE_IN(ctx, in, 1);
        IF_KW("ang-version", 11, LY_STMT_YANG_VERSION)
        else IF_KW("in-element", 10, LY_STMT_YIN_ELEMENT)
        break;
    default:
        /* if context is not NULL we are matching keyword from YANG data*/
        if (ctx) {
            if (in->current[0] == ';') {
                MOVE_IN(ctx, in, 1);
                *kw = LY_STMT_SYNTAX_SEMICOLON;
            } else if (in->current[0] == '{') {
                MOVE_IN(ctx, in, 1);
                *kw = LY_STMT_SYNTAX_LEFT_BRACE;
            } else if (in->current[0] == '}') {
                MOVE_IN(ctx, in, 1);
                *kw = LY_STMT_SYNTAX_RIGHT_BRACE;
            }
        }
        break;
    }

    if ((*kw < LY_STMT_SYNTAX_SEMICOLON) && isalnum(in->current[0])) {
        /* the keyword is not terminated */
        *kw = LY_STMT_NONE;
        in->current = start;
    }

#undef IF_KW
#undef IF_KW_PREFIX
#undef IF_KW_PREFIX_END
#undef MOVE_IN
    /* *INDENT-ON* */

    return result;
}

LY_ARRAY_COUNT_TYPE
lysp_ext_instance_iter(struct lysp_ext_instance *ext, LY_ARRAY_COUNT_TYPE index, LYEXT_SUBSTMT substmt)
{
    LY_CHECK_ARG_RET(NULL, ext, LY_EINVAL);

    for ( ; index < LY_ARRAY_COUNT(ext); index++) {
        if (ext[index].insubstmt == substmt) {
            return index;
        }
    }

    return LY_ARRAY_COUNT(ext);
}

const struct lysc_node *
lysc_data_parent(const struct lysc_node *schema)
{
    const struct lysc_node *parent;

    for (parent = schema->parent; parent && (parent->nodetype & (LYS_CHOICE | LYS_CASE)); parent = parent->parent) {}

    return parent;
}

ly_bool
lysc_is_output(const struct lysc_node *schema)
{
    const struct lysc_node *parent;

    assert(schema);

    for (parent = schema->parent; parent && !(parent->nodetype & (LYS_RPC | LYS_ACTION)); parent = parent->parent) {}
    if (parent && (schema->flags & LYS_CONFIG_R)) {
        return 1;
    }
    return 0;
}

API ly_bool
lysc_is_userordered(const struct lysc_node *schema)
{
    if (!schema || !(schema->nodetype & (LYS_LEAFLIST | LYS_LIST)) || !(schema->flags & LYS_ORDBY_USER)) {
        return 0;
    }

    return 1;
}
