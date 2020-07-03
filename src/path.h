/**
 * @file path.h
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief Path structure and manipulation routines.
 *
 * Copyright (c) 2020 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#ifndef LY_PATH_H_
#define LY_PATH_H_

#include <stddef.h>
#include <stdint.h>

#include "log.h"
#include "tree.h"
#include "tree_data.h"

struct ly_ctx;
struct lys_module;
struct lysc_node;
struct lyxp_expr;

enum ly_path_pred_type {
    LY_PATH_PREDTYPE_NONE = 0,  /**< no predicate */
    LY_PATH_PREDTYPE_POSITION,  /**< position predicate - [2] */
    LY_PATH_PREDTYPE_LIST,      /**< keys predicate - [key1='val1'][key2='val2']... */
    LY_PATH_PREDTYPE_LEAFLIST   /**< leaflist value predicate - [.='value'] */
};

/**
 * @brief Structure for holding one segment of resolved path on schema including simple predicates.
 * Is used as a [sized array](@ref sizedarrays).
 */
struct ly_path {
    const struct lysc_node *node; /**< Schema node representing the path segment, first node has special meaning:
                                       - is a top-level node - path is absolute,
                                       - is inner node - path is relative */
    struct ly_path_predicate {
        union {
            uint64_t position;    /**< position value for the position-predicate */
            struct {
                const struct lysc_node *key;    /**< key node of the predicate, NULL in case of a leaf-list predicate */
                struct lyd_value value; /**< value representation according to the key's type */
            };
        };
    } *predicates;            /**< [Sized array](@ref sizedarrays) of the path segment's predicates */
    enum ly_path_pred_type pred_type;   /**< Predicate type (see YANG ABNF) */
};

/**
 * @defgroup path_begin_options Path begin options.
 * @{
 */
#define LY_PATH_BEGIN_ABSOLUTE  0x01    /**< path must be absolute */
#define LY_PATH_BEGIN_EITHER    0x02    /**< path be be iether absolute or relative */
/** @} */

/**
 * @defgroup path_lref_options Path leafref options.
 * @{
 */
#define LY_PATH_LREF_FALSE      0x04    /**< path does not represent leafref */
#define LY_PATH_LREF_TRUE       0x08    /* '..' in path allowed, special leafref predicates expected (but are not compiled),
                                           implement traversed modules */
/** @} */

/**
 * @defgroup path_prefix_options Path prefix options.
 * @{
 */
#define LY_PATH_PREFIX_OPTIONAL     0x10    /**< prefixes in the path are optional */
#define LY_PATH_PREFIX_MANDATORY    0x20    /**< prefixes in the path are mandatory (XML insatnce-identifier) */
/** @} */

/**
 * @defgroup path_pred_options Path predicate options.
 * @{
 */
#define LY_PATH_PRED_KEYS       0x40 /* expected predicate only - [node='value']* */
#define LY_PATH_PRED_SIMPLE     0x80 /* expected predicates - [node='value']*; [.='value']; [1] */
#define LY_PATH_PRED_LEAFREF    0xC0 /* expected predicates only leafref - [node=current()/../../../node/node];
                                        at least 1 ".." and 1 "node" after */
/** @} */

/**
 * @brief Parse path into XPath token structure and perform all additional checks.
 *
 * @param[in] ctx libyang context.
 * @param[in] str_path Path to parse.
 * @param[in] path_len Length of @p str_path.
 * @param[in] begin Begin option (@ref path_begin_options).
 * @param[in] lref Lref option (@ref path_lref_options).
 * @param[in] prefix Prefix option (@ref path_prefix_options).
 * @param[in] pred Predicate option (@ref path_pred_options).
 * @param[out] expr Parsed path.
 * @return LY_ERR value.
 */
LY_ERR ly_path_parse(const struct ly_ctx *ctx, const char *str_path, size_t path_len, uint8_t begin, uint8_t lref,
                     uint8_t prefix, uint8_t pred, struct lyxp_expr **expr);

/**
 * @brief Parse predicate into XPath token structure and perform all additional checks.
 *
 * @param[in] ctx libyang context.
 * @param[in] str_path Path to parse.
 * @param[in] path_len Length of @p str_path.
 * @param[in] prefix Prefix option (@ref path_prefix_options).
 * @param[in] pred Predicate option (@ref path_pred_options).
 * @param[out] expr Parsed path.
 * @return LY_ERR value.
 */
LY_ERR ly_path_parse_predicate(const struct ly_ctx *ctx, const char *str_path, size_t path_len, uint8_t prefix,
                               uint8_t pred, struct lyxp_expr **expr);

/**
 * @defgroup path_oper_options Path operation options.
 * @{
 */
#define LY_PATH_OPER_INPUT  0x01    /**< if any RPC/action is traversed, its input nodes are used */
#define LY_PATH_OPER_OUTPUT 0x02    /**< if any RPC/action is traversed, its output nodes are used */
/** @} */

/* lref */

/**
 * @defgroup path_target_options Path target options.
 * @{
 */
#define LY_PATH_TARGET_SINGLE  0x10    /**< last (target) node must identify an exact instance */
#define LY_PATH_TARGET_MANY    0x20    /**< last (target) node may identify all instances (of leaf-list/list) */
/** @} */

/**
 * @brief Compile path into ly_path structure. Any predicates of a leafref are only checked, not compiled.
 *
 * @param[in] ctx libyang context.
 * @param[in] cur_mod Module of the current (original context) node. Used for nodes without prefix for ::LYD_SCHEMA format.
 * @param[in] ctx_node Context node. Can be NULL for absolute paths.
 * @param[in] expr Parsed path.
 * @param[in] lref Lref option (@ref path_lref_options).
 * @param[in] oper Oper option (@ref path_oper_options).
 * @param[in] target Target option (@ref path_target_options).
 * @param[in] resolve_prefix Callback for prefix resolution.
 * @param[in] prefix_data Data for @p resolve_prefix.
 * @param[in] format Format of the path.
 * @param[out] path Compiled path.
 * @return LY_ERR value.
 */
LY_ERR ly_path_compile(const struct ly_ctx *ctx, const struct lys_module *cur_mod, const struct lysc_node *ctx_node,
                       const struct lyxp_expr *expr, uint8_t lref, uint8_t oper, uint8_t target,
                       ly_clb_resolve_prefix resolve_prefix, void *prefix_data, LYD_FORMAT format, struct ly_path **path);

/**
 * @brief Compile predicate into ly_path_predicate structure. Only simple predicates (not leafref) are supported.
 *
 * @param[in] ctx libyang context.
 * @param[in] cur_mod Module of the current (original context) node. Used for nodes without prefix for ::LYD_SCHEMA format.
 * @param[in] ctx_node Context node, node for which the predicate is defined.
 * @param[in] expr Parsed path.
 * @param[in,out] tok_idx Index in @p expr, is adjusted for parsed tokens.
 * @param[in] resolve_prefix Callback for prefix resolution.
 * @param[in] prefix_data Data for @p resolve_prefix.
 * @param[in] format Format of the path.
 * @param[out] predicates Compiled predicates.
 * @param[out] pred_type Type of the compiled predicate(s).
 * @return LY_ERR value.
 */
LY_ERR ly_path_compile_predicate(const struct ly_ctx *ctx, const struct lys_module *cur_mod,
                                 const struct lysc_node *ctx_node, const struct lyxp_expr *expr, uint16_t *tok_idx,
                                 ly_clb_resolve_prefix resolve_prefix, void *prefix_data, LYD_FORMAT format,
                                 struct ly_path_predicate **predicates, enum ly_path_pred_type *pred_type);

/**
 * @brief Resolve at least partially the target defined by ly_path structure. Not supported for leafref!
 *
 * @param[in] path Path structure specifying the target.
 * @param[in] start Starting node for relative paths, can be any for absolute paths.
 * @param[out] path_idx Last found path segment index, can be NULL, set to 0 if not found.
 * @param[out] match Last found matching node, can be NULL, set to NULL if not found.
 * @return LY_ENOTFOUND if no nodes were found,
 * @return LY_EINCOMPLETE if some node was found but not the last one,
 * @return LY_SUCCESS when the last node in the path was found,
 * @return LY_ERR on another error.
 */
LY_ERR ly_path_eval_partial(const struct ly_path *path, const struct lyd_node *start, LY_ARRAY_COUNT_TYPE *path_idx,
                            struct lyd_node **match);

/**
 * @brief Resolve the target defined by ly_path structure. Not supported for leafref!
 *
 * @param[in] path Path structure specifying the target.
 * @param[in] start Starting node for relative paths, can be any for absolute paths.
 * @param[out] match Found matching node, can be NULL, set to NULL if not found.
 * @return LY_ENOTFOUND if no nodes were found,
 * @return LY_SUCCESS when the last node in the path was found,
 * @return LY_ERR on another error.
 */
LY_ERR ly_path_eval(const struct ly_path *path, const struct lyd_node *start, struct lyd_node **match);

/**
 * @brief Duplicate ly_path structure.
 *
 * @param[in] ctx libyang context.
 * @param[in] path Path to duplicate.
 * @param[out] dup Duplicated path.
 * @return LY_ERR value.
 */
LY_ERR ly_path_dup(const struct ly_ctx *ctx, const struct ly_path *path, struct ly_path **dup);

/**
 * @brief Free ly_path_predicate structure.
 *
 * @param[in] ctx libyang context.
 * @param[in] pred_type Predicate type.
 * @param[in] llist Leaf-list in case of leaf-list predicate.
 * @param[in] predicates Predicates ([sized array](@ref sizedarrays)) to free.
 */
void ly_path_predicates_free(const struct ly_ctx *ctx, enum ly_path_pred_type pred_type, const struct lysc_node *llist,
                             struct ly_path_predicate *predicates);

/**
 * @brief Free ly_path structure.
 *
 * @param[in] ctx libyang context.
 * @param[in] path The structure ([sized array](@ref sizedarrays)) to free.
 */
void ly_path_free(const struct ly_ctx *ctx, struct ly_path *path);

#endif /* LY_PATH_H_ */
