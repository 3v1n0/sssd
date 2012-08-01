/*
    SSSD

    IPA Subdomains Module

    Authors:
        Sumit Bose <sbose@redhat.com>

    Copyright (C) 2011 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "providers/ldap/sdap_async.h"
#include "providers/ipa/ipa_subdomains.h"
#include "providers/ipa/ipa_common.h"
#include <ctype.h>

#define SUBDOMAINS_FILTER "objectclass=ipaNTTrustedDomain"
#define MASTER_DOMAIN_FILTER "objectclass=ipaNTDomainAttrs"
#define RANGE_FILTER "objectclass=ipaIDRange"

#define IPA_CN "cn"
#define IPA_FLATNAME "ipaNTFlatName"
#define IPA_SID "ipaNTSecurityIdentifier"
#define IPA_TRUSTED_DOMAIN_SID "ipaNTTrustedDomainSID"

#define IPA_BASE_ID "ipaBaseID"
#define IPA_ID_RANGE_SIZE "ipaIDRangeSize"
#define IPA_BASE_RID "ipaBaseRID"
#define IPA_SECONDARY_BASE_RID "ipaSecondaryBaseRID"
#define OBJECTCLASS "objectClass"

/* do not refresh more often than every 5 seconds for now */
#define IPA_SUBDOMAIN_REFRESH_LIMIT 5

enum ipa_subdomains_req_type {
    IPA_SUBDOMAINS_MASTER,
    IPA_SUBDOMAINS_SLAVE,
    IPA_SUBDOMAINS_RANGES,

    IPA_SUBDOMAINS_MAX /* Counter */
};

struct ipa_subdomains_req_params {
    const char *filter;
    tevent_req_fn cb;
    const char *attrs[8];
};

struct ipa_subdomains_ctx {
    struct sdap_id_ctx *sdap_id_ctx;
    struct sdap_search_base **search_bases;
    struct sdap_search_base **master_search_bases;
    struct sdap_search_base **ranges_search_bases;

    /* subdomain map cache */
    time_t last_refreshed;
    int num_subdoms;
    struct sysdb_subdom *subdoms;
};

static void ipa_subdomains_reply(struct be_req *be_req, int dp_err, int result)
{
    be_req->fn(be_req, dp_err, result, NULL);
}

static errno_t ipa_ranges_parse_results(TALLOC_CTX *mem_ctx,
                                        size_t count,
                                        struct sysdb_attrs **reply,
                                        struct range_info ***_range_list)
{
    struct range_info **range_list = NULL;
    const char *value;
    size_t c;
    int ret;

    range_list = talloc_array(mem_ctx, struct range_info *, count + 1);
    if (range_list == NULL) {
        DEBUG(SSSDBG_OP_FAILURE, ("talloc_array failed.\n"));
        return ENOMEM;
    }

    for (c = 0; c < count; c++) {
        range_list[c] = talloc_zero(range_list, struct range_info);
        if (range_list[c] == NULL) {
            DEBUG(SSSDBG_OP_FAILURE, ("talloc_zero failed.\n"));
            ret = ENOMEM;
            goto done;
        }

        ret = sysdb_attrs_get_string(reply[c], IPA_CN, &value);
        if (ret != EOK) {
            DEBUG(SSSDBG_OP_FAILURE, ("sysdb_attrs_get_string failed.\n"));
            goto done;
        }
        range_list[c]->name = talloc_strdup(range_list[c], value);
        if (range_list[c]->name == NULL) {
            DEBUG(SSSDBG_OP_FAILURE, ("talloc_strdup failed.\n"));
            ret = ENOMEM;
            goto done;
        }

        ret = sysdb_attrs_get_string(reply[c], IPA_TRUSTED_DOMAIN_SID, &value);
        if (ret == EOK) {
            range_list[c]->trusted_dom_sid = talloc_strdup(range_list[c],
                                                           value);
            if (range_list[c]->trusted_dom_sid == NULL) {
                DEBUG(SSSDBG_OP_FAILURE, ("talloc_strdup failed.\n"));
                ret = ENOMEM;
                goto done;
            }
        } else if (ret != ENOENT) {
            DEBUG(SSSDBG_OP_FAILURE, ("sysdb_attrs_get_string failed.\n"));
            goto done;
        }

        ret = sysdb_attrs_get_uint32_t(reply[c], IPA_BASE_ID,
                                       &range_list[c]->base_id);
        if (ret != EOK && ret != ENOENT) {
            DEBUG(SSSDBG_OP_FAILURE, ("sysdb_attrs_get_string failed.\n"));
            goto done;
        }

        ret = sysdb_attrs_get_uint32_t(reply[c], IPA_ID_RANGE_SIZE,
                                       &range_list[c]->id_range_size);
        if (ret != EOK && ret != ENOENT) {
            DEBUG(SSSDBG_OP_FAILURE, ("sysdb_attrs_get_string failed.\n"));
            goto done;
        }

        ret = sysdb_attrs_get_uint32_t(reply[c], IPA_BASE_RID,
                                       &range_list[c]->base_rid);
        if (ret != EOK && ret != ENOENT) {
            DEBUG(SSSDBG_OP_FAILURE, ("sysdb_attrs_get_string failed.\n"));
            goto done;
        }

        ret = sysdb_attrs_get_uint32_t(reply[c], IPA_SECONDARY_BASE_RID,
                                       &range_list[c]->secondary_base_rid);
        if (ret != EOK && ret != ENOENT) {
            DEBUG(SSSDBG_OP_FAILURE, ("sysdb_attrs_get_string failed.\n"));
            goto done;
        }
    }
    range_list[c] = NULL;

    *_range_list = range_list;
    ret = EOK;

done:
    if (ret != EOK) {
        talloc_free(range_list);
    }

    return ret;
}

static char *name_to_realm(TALLOC_CTX *memctx, const char *name)
{
    char *realm;
    char *p;

    realm = talloc_strdup(memctx, name);
    if (!realm) {
        return NULL;
    }
    for (p = realm; *p; p++) {
        *p = toupper(*p);
    }

    return realm;
}

static errno_t ipa_subdom_parse(TALLOC_CTX *memctx,
                                struct sysdb_attrs *attrs,
                                struct sysdb_subdom *subdom)
{
    const char *value;
    int ret;

    ret = sysdb_attrs_get_string(attrs, IPA_CN, &value);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, ("sysdb_attrs_get_string failed.\n"));
        return ret;
    }
    if (subdom->name == NULL) {
        subdom->name = talloc_strdup(memctx, value);
        if (subdom->name == NULL) {
            return ENOMEM;
        }
    } else if (strcmp(subdom->name, value) != 0) {
        DEBUG(SSSDBG_OP_FAILURE, ("subdomain name mismatch!\n"));
        return EINVAL;
    }

    if (subdom->realm == NULL) {
        /* Add Realm as upper(domain name), this is generally always correct
         * with AD domains */
        subdom->realm = name_to_realm(memctx, subdom->name);
        if (!subdom->realm) {
            return ENOMEM;
        }
    }

    ret = sysdb_attrs_get_string(attrs, IPA_FLATNAME, &value);
    if (ret) {
        DEBUG(SSSDBG_OP_FAILURE, ("sysdb_attrs_get_string failed.\n"));
        return ret;
    }

    /* in theory this may change, it should never happen, so we will log a
     * warning if it does, but we will allow it for now */
    if (subdom->flat_name != NULL) {
        if (strcmp(subdom->flat_name, value) != 0) {
            DEBUG(SSSDBG_TRACE_INTERNAL,
                  ("Flat name for subdomain changed!\n"));
            talloc_free(discard_const(subdom->flat_name));
            subdom->flat_name = (const char *)NULL;
        }
    }
    if (subdom->flat_name == NULL) {
        subdom->flat_name = talloc_strdup(memctx, value);
        if (subdom->flat_name == NULL) {
            return ENOMEM;
        }
    }

    ret = sysdb_attrs_get_string(attrs, IPA_TRUSTED_DOMAIN_SID, &value);
    if (ret) {
        DEBUG(SSSDBG_OP_FAILURE, ("sysdb_attrs_get_string failed.\n"));
        return ret;
    }

    /* in theory this may change, it should never happen, so we will log a
     * warning if it does, but we will allow it for now */
    if (subdom->id != NULL) {
        if (strcmp(subdom->id, value) != 0) {
            DEBUG(SSSDBG_TRACE_INTERNAL,
                  ("ID for subdomain changed!\n"));
            talloc_free(discard_const(subdom->id));
            subdom->flat_name = (const char *)NULL;
        }
    }
    if (subdom->id == NULL) {
        subdom->id = talloc_strdup(memctx, value);
        if (subdom->id == NULL) {
            return ENOMEM;
        }
    }

    return EOK;
}

static errno_t ipa_subdomains_refresh(struct ipa_subdomains_ctx *ctx,
                                      int count, struct sysdb_attrs **reply,
                                      bool *changes)
{
    bool handled[count];
    const char *value;
    int c, h;
    int ret;
    int i, j;

    memset(handled, 0, sizeof(bool) * count);

    /* check existing subdoms in cache */
    for (i = 0, h = 0; i < ctx->num_subdoms; i++) {
        for (c = 0; c < count; c++) {
            if (handled[c]) {
                continue;
            }
            ret = sysdb_attrs_get_string(reply[c], IPA_CN, &value);
            if (ret != EOK) {
                DEBUG(SSSDBG_OP_FAILURE, ("sysdb_attrs_get_string failed.\n"));
                goto done;
            }
            if (strcmp(value, ctx->subdoms[i].name) == 0) {
                break;
            }
        }

        if (c >= count) {
            /* ok this subdomain does not exist anymore, let's clean up */
            for (j = i; j < ctx->num_subdoms - 1; j++) {
                ctx->subdoms[j] = ctx->subdoms[j + 1];
            }
            ctx->num_subdoms--;
            i--;
        } else {
            /* ok let's try to update it */
            ret = ipa_subdom_parse(ctx->subdoms, reply[c], &ctx->subdoms[i]);
            if (ret) {
                DEBUG(SSSDBG_OP_FAILURE, ("Failed to parse subdom data\n"));
                goto done;
            }
            handled[c] = true;
            h++;
        }
    }

    if (count == h) {
        /* all domains were already accounted for and have been updated */
        ret = EOK;
        goto done;
    }

    /* if we get here it means we have changes to the subdomains list */
    *changes = true;

    /* add space for unhandled domains */
    c = count - h;
    ctx->subdoms = talloc_realloc(ctx, ctx->subdoms,
                                  struct sysdb_subdom,
                                  ctx->num_subdoms + c);
    if (ctx->subdoms == NULL) {
        ret = ENOMEM;
        goto done;
    }

    for (c = 0; c < count; c++) {
        if (handled[c]) {
            continue;
        }
        i = ctx->num_subdoms;
        memset(&ctx->subdoms[i], 0, sizeof(struct sysdb_subdom));
        ret = ipa_subdom_parse(ctx->subdoms, reply[c], &ctx->subdoms[i]);
        if (ret) {
            DEBUG(SSSDBG_OP_FAILURE, ("Failed to parse subdom data\n"));
            goto done;
        }
        ctx->num_subdoms++;
    }

    ret = EOK;

done:
    if (ret != EOK) {
        ctx->last_refreshed = 0;
        ctx->num_subdoms = 0;
        talloc_zfree(ctx->subdoms);
    } else {
        ctx->last_refreshed = time(NULL);
    }

    return ret;
}

struct ipa_subdomains_req_ctx {
    struct be_req *be_req;
    struct ipa_subdomains_ctx *sd_ctx;
    struct sdap_id_op *sdap_op;
    struct be_subdom_req *sd_data;

    char *current_filter;

    struct sdap_search_base **search_bases;
    int search_base_iter;

    size_t reply_count;
    struct sysdb_attrs **reply;
};

static void ipa_subdomains_get_conn_done(struct tevent_req *req);
static errno_t
ipa_subdomains_handler_get(struct ipa_subdomains_req_ctx *ctx,
                           enum ipa_subdomains_req_type type);
static void ipa_subdomains_handler_done(struct tevent_req *req);
static void ipa_subdomains_handler_master_done(struct tevent_req *req);
static void ipa_subdomains_handler_ranges_done(struct tevent_req *req);

static struct ipa_subdomains_req_params subdomain_requests[] = {
    { MASTER_DOMAIN_FILTER,
      ipa_subdomains_handler_master_done,
      { IPA_CN, IPA_FLATNAME, IPA_SID, NULL }
    },
    { SUBDOMAINS_FILTER,
      ipa_subdomains_handler_done,
      { IPA_CN, IPA_FLATNAME, IPA_TRUSTED_DOMAIN_SID, NULL }
    },
    { RANGE_FILTER,
      ipa_subdomains_handler_ranges_done,
      { OBJECTCLASS, IPA_CN,
        IPA_BASE_ID, IPA_BASE_RID, IPA_SECONDARY_BASE_RID,
        IPA_ID_RANGE_SIZE, IPA_TRUSTED_DOMAIN_SID, NULL
      }
    }
};

void ipa_subdomains_handler(struct be_req *be_req)
{
    struct tevent_req *req;
    struct ipa_subdomains_req_ctx *req_ctx = NULL;
    struct ipa_subdomains_ctx *ctx;
    int dp_error = DP_ERR_FATAL;
    int ret;

    ctx = talloc_get_type(be_req->be_ctx->bet_info[BET_SUBDOMAINS].pvt_bet_data,
                          struct ipa_subdomains_ctx);
    if (!ctx) {
        ret = EINVAL;
        goto done;
    }

    if (ctx->last_refreshed > time(NULL) - IPA_SUBDOMAIN_REFRESH_LIMIT) {
        ret = EOK;
        goto done;
    }

    req_ctx = talloc(be_req, struct ipa_subdomains_req_ctx);
    if (req_ctx == NULL) {
        ret = ENOMEM;
        goto done;
    }

    req_ctx->be_req = be_req;
    req_ctx->sd_ctx = ctx;
    req_ctx->sd_data = talloc_get_type(be_req->req_data, struct be_subdom_req);
    req_ctx->search_base_iter = 0;
    req_ctx->search_bases = ctx->search_bases;
    req_ctx->current_filter = NULL;
    req_ctx->reply_count = 0;
    req_ctx->reply = NULL;

    req_ctx->sdap_op = sdap_id_op_create(req_ctx,
                                         ctx->sdap_id_ctx->conn_cache);
    if (req_ctx->sdap_op == NULL) {
        DEBUG(SSSDBG_OP_FAILURE, ("sdap_id_op_create failed.\n"));
        ret = ENOMEM;
        goto done;
    }

    req = sdap_id_op_connect_send(req_ctx->sdap_op, req_ctx, &ret);
    if (req == NULL) {
        DEBUG(SSSDBG_OP_FAILURE, ("sdap_id_op_connect_send failed: %d(%s).\n",
                                  ret, strerror(ret)));
        goto done;
    }

    tevent_req_set_callback(req, ipa_subdomains_get_conn_done, req_ctx);

    return;

done:
    talloc_free(req_ctx);
    if (ret == EOK) {
        dp_error = DP_ERR_OK;
    }
    ipa_subdomains_reply(be_req, dp_error, ret);
}

static void ipa_subdomains_get_conn_done(struct tevent_req *req)
{
    int ret;
    int dp_error = DP_ERR_FATAL;
    struct be_req *be_req;
    struct ipa_subdomains_req_ctx *ctx;

    ctx = tevent_req_callback_data(req, struct ipa_subdomains_req_ctx);

    ret = sdap_id_op_connect_recv(req, &dp_error);
    talloc_zfree(req);
    if (ret) {
        if (dp_error == DP_ERR_OFFLINE) {
            DEBUG(SSSDBG_MINOR_FAILURE,
                  ("No IPA server is available, cannot get the "
                   "subdomain list while offline\n"));

/* FIXME: return saved results ?? */
        } else {
            DEBUG(SSSDBG_OP_FAILURE,
                  ("Failed to connect to IPA server: [%d](%s)\n",
                   ret, strerror(ret)));
        }

        goto fail;
    }

    ret = ipa_subdomains_handler_get(ctx, IPA_SUBDOMAINS_SLAVE);
    if (ret != EOK && ret != EAGAIN) {
        goto fail;
    }

    return;

fail:
    be_req = ctx->be_req;
    talloc_free(ctx);
    ipa_subdomains_reply(be_req, dp_error, ret);
}

static errno_t
ipa_subdomains_handler_get(struct ipa_subdomains_req_ctx *ctx,
                           enum ipa_subdomains_req_type type)
{
    struct tevent_req *req;
    struct sdap_search_base *base;
    struct ipa_subdomains_req_params *params;

    if (type >= IPA_SUBDOMAINS_MAX) {
        return EINVAL;
    }

    params = &subdomain_requests[type];

    base = ctx->search_bases[ctx->search_base_iter];
    if (base == NULL) {
        return EOK;
    }

    talloc_free(ctx->current_filter);
    ctx->current_filter = sdap_get_id_specific_filter(ctx, params->filter,
                                                            base->filter);
    if (ctx->current_filter == NULL) {
        return ENOMEM;
    }

    req = sdap_get_generic_send(ctx, ctx->be_req->be_ctx->ev,
                        ctx->sd_ctx->sdap_id_ctx->opts,
                        sdap_id_op_handle(ctx->sdap_op),
                        base->basedn, base->scope,
                        ctx->current_filter, params->attrs, NULL, 0,
                        dp_opt_get_int(ctx->sd_ctx->sdap_id_ctx->opts->basic,
                                       SDAP_SEARCH_TIMEOUT), false);

    if (req == NULL) {
        DEBUG(SSSDBG_OP_FAILURE, ("sdap_get_generic_send failed.\n"));
        return ENOMEM;
    }

    tevent_req_set_callback(req, params->cb, ctx);

    return EAGAIN;
}

static void ipa_subdomains_handler_done(struct tevent_req *req)
{
    int ret;
    size_t reply_count;
    struct sysdb_attrs **reply = NULL;
    struct ipa_subdomains_req_ctx *ctx;
    struct be_req *be_req;
    struct sysdb_ctx *sysdb;
    bool refresh_has_changes = false;

    ctx = tevent_req_callback_data(req, struct ipa_subdomains_req_ctx);
    be_req = ctx->be_req;
    if (be_req->sysdb) {
        sysdb = be_req->sysdb;
    } else {
        sysdb = be_req->be_ctx->sysdb;
    }

    ret = sdap_get_generic_recv(req, ctx, &reply_count, &reply);
    talloc_zfree(req);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, ("sdap_get_generic_send request failed.\n"));
        goto done;
    }

    if (reply_count) {
        ctx->reply = talloc_realloc(ctx, ctx->reply, struct sysdb_attrs *,
                                    ctx->reply_count + reply_count);
        if (ctx->reply == NULL) {
            ret = ENOMEM;
            goto done;
        }
        memcpy(ctx->reply+ctx->reply_count, reply,
               reply_count * sizeof(struct sysdb_attrs *));
        ctx->reply_count += reply_count;
    }

    ctx->search_base_iter++;
    ret = ipa_subdomains_handler_get(ctx, IPA_SUBDOMAINS_SLAVE);
    if (ret == EAGAIN) {
        return;
    } else if (ret != EOK) {
        goto done;
    }

    ret = ipa_subdomains_refresh(ctx->sd_ctx, ctx->reply_count, ctx->reply,
                                 &refresh_has_changes);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, ("Failed to refresh subdomains.\n"));
        goto done;
    }

    if (refresh_has_changes) {
        ret = sysdb_update_subdomains(sysdb, ctx->sd_ctx->num_subdoms,
                                      ctx->sd_ctx->subdoms);
        if (ret != EOK) {
            DEBUG(SSSDBG_OP_FAILURE, ("sysdb_update_subdomains failed.\n"));
            goto done;
        }
    }


    ctx->search_base_iter = 0;
    ctx->search_bases = ctx->sd_ctx->ranges_search_bases;
    ret = ipa_subdomains_handler_get(ctx, IPA_SUBDOMAINS_RANGES);
    if (ret == EAGAIN) {
        return;
    } else if (ret != EOK) {
        goto done;
    }

    DEBUG(SSSDBG_OP_FAILURE, ("No search base for ranges available.\n"));
    ret = EINVAL;

done:
    talloc_free(ctx);
    ipa_subdomains_reply(be_req, DP_ERR_FATAL, ret);
}


static void ipa_subdomains_handler_ranges_done(struct tevent_req *req)
{
    errno_t ret;
    int dp_error = DP_ERR_FATAL;
    size_t reply_count;
    struct sysdb_attrs **reply = NULL;
    struct ipa_subdomains_req_ctx *ctx;
    struct be_req *be_req;
    struct sysdb_subdom *domain_info;
    struct range_info **range_list = NULL;
    struct sysdb_ctx *sysdb;

    ctx = tevent_req_callback_data(req, struct ipa_subdomains_req_ctx);
    be_req = ctx->be_req;
    if (be_req->sysdb) {
        sysdb = be_req->sysdb;
    } else {
        sysdb = be_req->be_ctx->sysdb;
    }

    ret = sdap_get_generic_recv(req, ctx, &reply_count, &reply);
    talloc_zfree(req);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, ("sdap_get_generic_send request failed.\n"));
        goto done;
    }

    ret = ipa_ranges_parse_results(ctx, reply_count, reply, &range_list);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE,
              ("ipa_ranges_parse_results request failed.\n"));
        goto done;
    }

    ret = sysdb_update_ranges(sysdb, range_list);
    talloc_free(range_list);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, ("sysdb_update_ranges failed.\n"));
        goto done;
    }


    ret = sysdb_master_domain_get_info(ctx, sysdb, &domain_info);
    if (ret != EOK) {
        goto done;
    }

    if (domain_info->flat_name == NULL ||
        domain_info->id == NULL ||
        domain_info->name == NULL) {

        ctx->search_base_iter = 0;
        ctx->search_bases = ctx->sd_ctx->master_search_bases;
        ret = ipa_subdomains_handler_get(ctx, IPA_SUBDOMAINS_MASTER);
        if (ret == EAGAIN) {
            return;
        } else if (ret != EOK) {
            goto done;
        }
    } else {
        ret = EOK;
    }

done:
    talloc_free(ctx);
    if (ret == EOK) {
        dp_error = DP_ERR_OK;
    }
    ipa_subdomains_reply(be_req, dp_error, ret);
}

static void ipa_subdomains_handler_master_done(struct tevent_req *req)
{
    errno_t ret;
    int dp_error = DP_ERR_FATAL;
    size_t reply_count;
    struct sysdb_attrs **reply = NULL;
    struct ipa_subdomains_req_ctx *ctx;
    struct be_req *be_req;
    struct sysdb_subdom *domain_info;
    const char *tmp_str;

    ctx = tevent_req_callback_data(req, struct ipa_subdomains_req_ctx);
    be_req = ctx->be_req;

    ret = sdap_get_generic_recv(req, ctx, &reply_count, &reply);
    talloc_zfree(req);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, ("sdap_get_generic_send request failed.\n"));
        goto done;
    }

    if (reply_count) {
        domain_info = talloc_zero(ctx, struct sysdb_subdom);
        if (domain_info == NULL) {
            ret = ENOMEM;
            goto done;
        }

        ret = sysdb_attrs_get_string(reply[0], IPA_FLATNAME, &tmp_str);
        if (ret != EOK) {
            goto done;
        }
        domain_info->flat_name = talloc_strdup(domain_info, tmp_str);
        if (domain_info->flat_name == NULL) {
            ret = ENOMEM;
            goto done;
        }

        ret = sysdb_attrs_get_string(reply[0], IPA_SID, &tmp_str);
        if (ret != EOK) {
            goto done;
        }
        domain_info->id = talloc_strdup(domain_info, tmp_str);
        if (domain_info->id == NULL) {
            ret = ENOMEM;
            goto done;
        }

        ret = sysdb_master_domain_add_info(be_req->be_ctx->sysdb, domain_info);
        goto done;
    } else {
        ctx->search_base_iter++;
        ret = ipa_subdomains_handler_get(ctx, IPA_SUBDOMAINS_MASTER);
        if (ret == EAGAIN) {
            return;
        } else if (ret != EOK) {
            goto done;
        }

        /* Right now we know there has been an error
         * and we don't have the master domain record
         */
        DEBUG(SSSDBG_CRIT_FAILURE, ("Master domain record not found!\n"));
        ret = EIO;
        goto done;
    }

done:
    talloc_free(ctx);
    if (ret == EOK) {
        dp_error = DP_ERR_OK;
    }
    ipa_subdomains_reply(be_req, dp_error, ret);
}

struct bet_ops ipa_subdomains_ops = {
    .handler = ipa_subdomains_handler,
    .finalize = NULL
};

int ipa_subdom_init(struct be_ctx *be_ctx,
                    struct ipa_id_ctx *id_ctx,
                    struct bet_ops **ops,
                    void **pvt_data)
{
    struct ipa_subdomains_ctx *ctx;

    ctx = talloc_zero(id_ctx, struct ipa_subdomains_ctx);
    if (ctx == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("talloc_zero failed.\n"));
        return ENOMEM;
    }

    ctx->sdap_id_ctx = id_ctx->sdap_id_ctx;
    ctx->search_bases = id_ctx->ipa_options->subdomains_search_bases;
    ctx->master_search_bases = id_ctx->ipa_options->master_domain_search_bases;
    ctx->ranges_search_bases = id_ctx->ipa_options->ranges_search_bases;
    *ops = &ipa_subdomains_ops;
    *pvt_data = ctx;

    return EOK;
}
