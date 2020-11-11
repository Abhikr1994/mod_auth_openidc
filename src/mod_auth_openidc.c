/***************************************************************************
 *
 * Copyright (C) 2018-2019 - ZmartZone Holding BV - www.zmartzone.eu
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * @Author: Hans Zandbelt - hans.zandbelt@zmartzone.eu
 *
 **************************************************************************/

#include "mod_auth_openidc.h"

#include <oauth2/apache.h>
#include <oauth2/cfg.h>
#include <oauth2/mem.h>
#include <oauth2/oauth2.h>
#include <oauth2/openidc.h>
#include <oauth2/proto.h>
#include <oauth2/session.h>
#include <oauth2/util.h>

#include <httpd.h>

#include <http_config.h>
#include <http_core.h>
#include <http_log.h>
#include <http_request.h>

#include <apr_strings.h>

OAUTH2_APACHE_LOG(auth_openidc)

typedef struct openidc_cfg_dir_t {
	oauth2_cfg_openidc_t *openidc;
	oauth2_cfg_target_pass_t *target_pass;
} openidc_cfg_dir_t;

static apr_status_t openidc_cfg_dir_cleanup(void *data)
{
	openidc_cfg_dir_t *cfg = (openidc_cfg_dir_t *)data;
	oauth2_cfg_openidc_free(NULL, cfg->openidc);
	oauth2_cfg_target_pass_free(NULL, cfg->target_pass);
	oauth2_mem_free(cfg);
	return APR_SUCCESS;
}

static void *openidc_cfg_dir_create(apr_pool_t *pool, char *path)
{
	openidc_cfg_dir_t *cfg = oauth2_mem_alloc(sizeof(openidc_cfg_dir_t));
	oauth2_cfg_openidc_free(NULL, cfg->openidc);
	cfg->openidc = oauth2_cfg_openidc_init(NULL);
	cfg->target_pass = oauth2_cfg_target_pass_init(NULL);
	apr_pool_cleanup_register(pool, cfg, openidc_cfg_dir_cleanup,
				  openidc_cfg_dir_cleanup);
	return cfg;
}

static void *openidc_cfg_dir_merge(apr_pool_t *pool, void *b, void *a)
{
	openidc_cfg_dir_t *cfg = openidc_cfg_dir_create(pool, NULL);
	openidc_cfg_dir_t *base = b;
	openidc_cfg_dir_t *add = a;
	oauth2_cfg_openidc_merge(NULL, cfg->openidc, base->openidc,
				 add->openidc);
	oauth2_cfg_target_pass_merge(NULL, cfg->target_pass, base->target_pass,
				     add->target_pass);
	return cfg;
}

static const char *
openidc_cfg_set_openidc_provider_resolver(cmd_parms *cmd, void *m,
					  const char *type, const char *value,
					  const char *options)
{
	const char *rv = NULL;
	openidc_cfg_dir_t *dir_cfg = NULL;
	oauth2_apache_cfg_srv_t *srv_cfg = NULL;

	dir_cfg = (openidc_cfg_dir_t *)m;
	srv_cfg = ap_get_module_config(cmd->server->module_config,
				       &auth_openidc_module);

	rv = oauth2_cfg_openidc_provider_resolver_set_options(
	    srv_cfg->log, dir_cfg->openidc, type, value, options);

	return rv;
}

// TODO: MACRO-IZE across mod_oauth.c
static const char *openidc_cfg_set_target_pass(cmd_parms *cmd, void *m,
					       const char *options)
{
	const char *rv = NULL;
	openidc_cfg_dir_t *dir_cfg = NULL;
	oauth2_apache_cfg_srv_t *srv_cfg = NULL;

	dir_cfg = (openidc_cfg_dir_t *)m;
	srv_cfg = ap_get_module_config(cmd->server->module_config,
				       &auth_openidc_module);
	rv = oauth2_cfg_set_target_pass_options(srv_cfg->log,
						dir_cfg->target_pass, options);
	return rv;
}

static const char *openidc_cfg_set_cache_mod(cmd_parms *cmd, void *m,
					     const char *type,
					     const char *options)
{
	const char *rv = NULL;
	oauth2_apache_cfg_srv_t *srv_cfg = ap_get_module_config(
	    cmd->server->module_config, &auth_openidc_module);
	rv = oauth2_cfg_set_cache(srv_cfg->log, type, options);
	return rv;
}

static const char *openidc_cfg_set_session_mod(cmd_parms *cmd, void *m,
					       const char *type,
					       const char *options)
{
	const char *rv = NULL;
	oauth2_apache_cfg_srv_t *srv_cfg = ap_get_module_config(
	    cmd->server->module_config, &auth_openidc_module);
	oauth2_cfg_session_t *session_cfg =
	    oauth2_cfg_session_init(srv_cfg->log);
	rv = oauth2_cfg_session_set_options(srv_cfg->log, session_cfg, type,
					    options);
	return rv;
}

// clang-format off

OAUTH2_APACHE_HANDLERS(auth_openidc)

#define OPENIDC_CFG_CMD_ARGS(nargs, cmd, member, desc) \
	AP_INIT_TAKE##nargs( \
		cmd, \
		openidc_cfg_set_##member, \
		NULL, \
		RSRC_CONF | ACCESS_CONF | OR_AUTHCFG, \
		desc)

static const command_rec OAUTH2_APACHE_COMMANDS(auth_openidc)[] = {

	OPENIDC_CFG_CMD_ARGS(12,
		"OpenIDCCache",
		cache_mod,
		"Set cache backend and options."),

	OPENIDC_CFG_CMD_ARGS(12,
		"OpenIDCSession",
		session_mod,
		"Set session backend and options."),

	AP_INIT_TAKE123(
		"OpenIDCProviderResolver",
		openidc_cfg_set_openidc_provider_resolver,
		NULL,
		RSRC_CONF | ACCESS_CONF | OR_AUTHCFG,
		"Configures a resolver for OpenID Connect Provider configuration data."),

	OPENIDC_CFG_CMD_ARGS(1,
		"OpenIDCTargetPass",
		target_pass,
		"Configures in which format claims are passed to the target application."),

	{ NULL }
};

// clang-format on

static int openidc_request_handler(oauth2_cfg_openidc_t *cfg,
				   oauth2_cfg_target_pass_t *target_pass,
				   oauth2_apache_request_ctx_t *ctx)
{
	int rv = DECLINED;
	bool rc = false;
	oauth2_http_response_t *response = NULL;
	json_t *claims = NULL;
	char *s_claims = NULL;

	oauth2_debug(ctx->log, "enter");

	oauth2_apache_scrub_headers(ctx, target_pass);

	rc = oauth2_openidc_handle(ctx->log, cfg, ctx->request, &response,
				   &claims);
	if (rc == false) {
		rv = HTTP_INTERNAL_SERVER_ERROR;
		goto end;
	}

	rv = oauth2_http_response_status_code_get(ctx->log, response);
	// TODO: HTTP_OK = 200?

	if (oauth2_apache_http_response_set(ctx->log, response, ctx->r) ==
	    false) {
		rv = HTTP_INTERNAL_SERVER_ERROR;
		goto end;
	}

	//	if (oauth2_apache_http_response_status_code_get() == 200)
	//		rv = OK;

	s_claims = oauth2_json_encode(ctx->log, claims, 0);
	oauth2_debug(ctx->log, "claims: %s", s_claims);

	if (claims) {
		// TODO:
		ctx->r->user = apr_pstrdup(
		    ctx->r->pool,
		    json_string_value(json_object_get(claims, "sub")));
		if (ctx->r->user == NULL)
			ctx->r->user = apr_pstrdup(ctx->r->pool, "(dummy)");
		oauth2_debug(ctx->log, "r->user: %s",
			     ctx->r->user ? ctx->r->user : "(null)");
		oauth2_apache_target_pass(ctx, target_pass, NULL, claims);
		rv = OK;
	}

end:

	if (s_claims)
		oauth2_mem_free(s_claims);
	if (claims)
		json_decref(claims);
	if (response)
		oauth2_http_response_free(ctx->log, response);

	return rv;
}

static int openidc_check_user_id_handler(request_rec *r)
{
	openidc_cfg_dir_t *cfg = NULL;
	oauth2_apache_request_ctx_t *ctx = NULL;

	if (ap_auth_type(r) == NULL)
		return DECLINED;

	if (ap_is_initial_req(r) == 0) {

		if (r->main != NULL)
			r->user = r->main->user;
		else if (r->prev != NULL)
			r->user = r->prev->user;

		if (r->user != NULL) {

			ap_log_rerror(
			    APLOG_MARK, APLOG_DEBUG, 0, r,
			    "recycling user '%s' from initial request "
			    "for sub-request",
			    r->user);

			return OK;
		}
	}

	cfg = ap_get_module_config(r->per_dir_config, &auth_openidc_module);
	ctx = OAUTH2_APACHE_REQUEST_CTX(r, auth_openidc);

	// TODO:
	oauth2_cfg_openidc_passphrase_set(ctx->log, cfg->openidc,
					  "password1234");

	oauth2_debug(ctx->log,
		     "incoming request: \"%s?%s\" ap_is_initial_req=%d",
		     r->parsed_uri.path, r->args, ap_is_initial_req(r));

	// TODO: don't really need oauth2_openidc_is_request_to_redirect_uri...
	if ((strcasecmp((const char *)ap_auth_type(r), OPENIDC_AUTH_TYPE) ==
	     0) ||
	    (strcasecmp((const char *)ap_auth_type(r),
			OPENIDC_AUTH_TYPE_OPENIDC) == 0) ||
	    (oauth2_openidc_is_request_to_redirect_uri(ctx->log, cfg->openidc,
						       ctx->request)))
		return openidc_request_handler(cfg->openidc, cfg->target_pass,
					       ctx);

	return DECLINED;
}

static void auth_openidc_register_hooks(apr_pool_t *p)
{
	static const char *const aszPre[] = {"mod_oauth2.c", NULL};
	ap_hook_post_config(OAUTH2_APACHE_POST_CONFIG(auth_openidc), aszPre,
			    NULL, APR_HOOK_MIDDLE);
	ap_hook_check_authn(openidc_check_user_id_handler, aszPre, NULL,
			    APR_HOOK_MIDDLE, AP_AUTH_INTERNAL_PER_CONF);
}

// clang-format off
OAUTH2_APACHE_MODULE_DECLARE_EX(
	auth_openidc,
	openidc_cfg_dir_create,
	openidc_cfg_dir_merge
)
// clang-format on
