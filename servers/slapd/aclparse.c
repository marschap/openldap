/* aclparse.c - routines to parse and check acl's */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1998-2005 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* Portions Copyright (c) 1995 Regents of the University of Michigan.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that this notice is preserved and that due credit is given
 * to the University of Michigan at Ann Arbor. The name of the University
 * may not be used to endorse or promote products derived from this
 * software without specific prior written permission. This software
 * is provided ``as is'' without express or implied warranty.
 */

#include "portable.h"

#include <stdio.h>

#include <ac/ctype.h>
#include <ac/regex.h>
#include <ac/socket.h>
#include <ac/string.h>
#include <ac/unistd.h>

#include "slap.h"
#include "lber_pvt.h"
#include "lutil.h"

static const char style_base[] = "base";
char *style_strings[] = {
	"regex",
	"expand",
	"exact",
	"one",
	"subtree",
	"children",
	"level",
	"attrof",
	"anonymous",
	"users",
	"self",
	"ip",
	"path",
	NULL
};

static void		split(char *line, int splitchar, char **left, char **right);
static void		access_append(Access **l, Access *a);
static void		acl_usage(void) LDAP_GCCATTR((noreturn));

static void		acl_regex_normalized_dn(const char *src, struct berval *pat);

#ifdef LDAP_DEBUG
static void		print_acl(Backend *be, AccessControl *a);
#endif

static int		check_scope( BackendDB *be, AccessControl *a );

#ifdef SLAP_DYNACL
static int
slap_dynacl_config( const char *fname, int lineno, Access *b, const char *name, slap_style_t sty, const char *right )
{
	slap_dynacl_t	*da, *tmp;
	int		rc = 0;

	for ( da = b->a_dynacl; da; da = da->da_next ) {
		if ( strcasecmp( da->da_name, name ) == 0 ) {
			fprintf( stderr,
				"%s: line %d: dynacl \"%s\" already specified.\n",
				fname, lineno, name );
			acl_usage();
		}
	}

	da = slap_dynacl_get( name );
	if ( da == NULL ) {
		return -1;
	}

	tmp = ch_malloc( sizeof( slap_dynacl_t ) );
	*tmp = *da;

	if ( tmp->da_parse ) {
		rc = ( *tmp->da_parse )( fname, lineno, sty, right, &tmp->da_private );
		if ( rc ) {
			ch_free( tmp );
			return rc;
		}
	}

	tmp->da_next = b->a_dynacl;
	b->a_dynacl = tmp;

	return 0;
}
#endif /* SLAP_DYNACL */

static void
regtest(const char *fname, int lineno, char *pat) {
	int e;
	regex_t re;

	char buf[512];
	unsigned size;

	char *sp;
	char *dp;
	int  flag;

	sp = pat;
	dp = buf;
	size = 0;
	buf[0] = '\0';

	for (size = 0, flag = 0; (size < sizeof(buf)) && *sp; sp++) {
		if (flag) {
			if (*sp == '$'|| (*sp >= '0' && *sp <= '9')) {
				*dp++ = *sp;
				size++;
			}
			flag = 0;

		} else {
			if (*sp == '$') {
				flag = 1;
			} else {
				*dp++ = *sp;
				size++;
			}
		}
	}

	*dp = '\0';
	if ( size >= (sizeof(buf) - 1) ) {
		fprintf( stderr,
			"%s: line %d: regular expression \"%s\" too large\n",
			fname, lineno, pat );
		acl_usage();
	}

	if ((e = regcomp(&re, buf, REG_EXTENDED|REG_ICASE))) {
		char error[512];
		regerror(e, &re, error, sizeof(error));
		fprintf( stderr,
			"%s: line %d: regular expression \"%s\" bad because of %s\n",
			fname, lineno, pat, error );
		acl_usage();
	}
	regfree(&re);
}

/*
 * Experimental
 *
 * Check if the pattern of an ACL, if any, matches the scope
 * of the backend it is defined within.
 */
#define	ACL_SCOPE_UNKNOWN	(-2)
#define	ACL_SCOPE_ERR		(-1)
#define	ACL_SCOPE_OK		(0)
#define	ACL_SCOPE_PARTIAL	(1)
#define	ACL_SCOPE_WARN		(2)

static int
check_scope( BackendDB *be, AccessControl *a )
{
	int		patlen;
	struct berval	dn;

	dn = be->be_nsuffix[0];

	if ( BER_BVISEMPTY( &dn ) ) {
		return ACL_SCOPE_OK;
	}

	if ( !BER_BVISEMPTY( &a->acl_dn_pat ) ||
			a->acl_dn_style != ACL_STYLE_REGEX )
	{
		slap_style_t	style = a->acl_dn_style;

		if ( style == ACL_STYLE_REGEX ) {
			char		dnbuf[SLAP_LDAPDN_MAXLEN + 2];
			char		rebuf[SLAP_LDAPDN_MAXLEN + 1];
			ber_len_t	rebuflen;
			regex_t		re;
			int		rc;
			
			/* add trailing '$' to database suffix to form
			 * a simple trial regex pattern "<suffix>$" */
			AC_MEMCPY( dnbuf, be->be_nsuffix[0].bv_val,
				be->be_nsuffix[0].bv_len );
			dnbuf[be->be_nsuffix[0].bv_len] = '$';
			dnbuf[be->be_nsuffix[0].bv_len + 1] = '\0';

			if ( regcomp( &re, dnbuf, REG_EXTENDED|REG_ICASE ) ) {
				return ACL_SCOPE_WARN;
			}

			/* remove trailing ')$', if any, from original
			 * regex pattern */
			rebuflen = a->acl_dn_pat.bv_len;
			AC_MEMCPY( rebuf, a->acl_dn_pat.bv_val, rebuflen + 1 );
			if ( rebuf[rebuflen - 1] == '$' ) {
				rebuf[--rebuflen] = '\0';
			}
			while ( rebuflen > be->be_nsuffix[0].bv_len && rebuf[rebuflen - 1] == ')' ) {
				rebuf[--rebuflen] = '\0';
			}
			if ( rebuflen == be->be_nsuffix[0].bv_len ) {
				rc = ACL_SCOPE_WARN;
				goto regex_done;
			}

			/* not a clear indication of scoping error, though */
			rc = regexec( &re, rebuf, 0, NULL, 0 )
				? ACL_SCOPE_WARN : ACL_SCOPE_OK;

regex_done:;
			regfree( &re );
			return rc;
		}

		patlen = a->acl_dn_pat.bv_len;
		/* If backend suffix is longer than pattern,
		 * it is a potential mismatch (in the sense
		 * that a superior naming context could
		 * match */
		if ( dn.bv_len > patlen ) {
			/* base is blatantly wrong */
			if ( style == ACL_STYLE_BASE ) return ACL_SCOPE_ERR;

			/* a style of one can be wrong if there is
			 * more than one level between the suffix
			 * and the pattern */
			if ( style == ACL_STYLE_ONE ) {
				int	rdnlen = -1, sep = 0;

				if ( patlen > 0 ) {
					if ( !DN_SEPARATOR( dn.bv_val[dn.bv_len - patlen - 1] )) {
						return ACL_SCOPE_ERR;
					}
					sep = 1;
				}

				rdnlen = dn_rdnlen( NULL, &dn );
				if ( rdnlen != dn.bv_len - patlen - sep )
					return ACL_SCOPE_ERR;
			}

			/* if the trailing part doesn't match,
			 * then it's an error */
			if ( strcmp( a->acl_dn_pat.bv_val,
				&dn.bv_val[dn.bv_len - patlen] ) != 0 )
			{
				return ACL_SCOPE_ERR;
			}

			return ACL_SCOPE_PARTIAL;
		}

		switch ( style ) {
		case ACL_STYLE_BASE:
		case ACL_STYLE_ONE:
		case ACL_STYLE_CHILDREN:
		case ACL_STYLE_SUBTREE:
			break;

		default:
			assert( 0 );
			break;
		}

		if ( dn.bv_len < patlen &&
			!DN_SEPARATOR( a->acl_dn_pat.bv_val[patlen - dn.bv_len - 1] ))
		{
			return ACL_SCOPE_ERR;
		}

		if ( strcmp( &a->acl_dn_pat.bv_val[patlen - dn.bv_len], dn.bv_val )
			!= 0 )
		{
			return ACL_SCOPE_ERR;
		}

		return ACL_SCOPE_OK;
	}

	return ACL_SCOPE_UNKNOWN;
}

void
parse_acl(
    Backend	*be,
    const char	*fname,
    int		lineno,
    int		argc,
    char	**argv,
	int		pos )
{
	int		i;
	char		*left, *right, *style, *next;
	struct berval	bv;
	AccessControl	*a;
	Access	*b;
	int rc;
	const char *text;

	a = NULL;
	for ( i = 1; i < argc; i++ ) {
		/* to clause - select which entries are protected */
		if ( strcasecmp( argv[i], "to" ) == 0 ) {
			if ( a != NULL ) {
				fprintf( stderr, "%s: line %d: "
					"only one to clause allowed in access line\n",
				    fname, lineno );
				acl_usage();
			}
			a = (AccessControl *) ch_calloc( 1, sizeof(AccessControl) );
			for ( ++i; i < argc; i++ ) {
				if ( strcasecmp( argv[i], "by" ) == 0 ) {
					i--;
					break;
				}

				if ( strcasecmp( argv[i], "*" ) == 0 ) {
					if ( !BER_BVISEMPTY( &a->acl_dn_pat ) ||
						a->acl_dn_style != ACL_STYLE_REGEX )
					{
						fprintf( stderr,
							"%s: line %d: dn pattern"
							" already specified in to clause.\n",
							fname, lineno );
						acl_usage();
					}

					ber_str2bv( "*", STRLENOF( "*" ), 1, &a->acl_dn_pat );
					continue;
				}

				split( argv[i], '=', &left, &right );
				split( left, '.', &left, &style );

				if ( right == NULL ) {
					fprintf( stderr, "%s: line %d: "
						"missing \"=\" in \"%s\" in to clause\n",
					    fname, lineno, left );
					acl_usage();
				}

				if ( strcasecmp( left, "dn" ) == 0 ) {
					if ( !BER_BVISEMPTY( &a->acl_dn_pat ) ||
						a->acl_dn_style != ACL_STYLE_REGEX )
					{
						fprintf( stderr,
							"%s: line %d: dn pattern"
							" already specified in to clause.\n",
							fname, lineno );
						acl_usage();
					}

					if ( style == NULL || *style == '\0' ||
						strcasecmp( style, "baseObject" ) == 0 ||
						strcasecmp( style, "base" ) == 0 ||
						strcasecmp( style, "exact" ) == 0 )
					{
						a->acl_dn_style = ACL_STYLE_BASE;
						ber_str2bv( right, 0, 1, &a->acl_dn_pat );

					} else if ( strcasecmp( style, "oneLevel" ) == 0 ||
						strcasecmp( style, "one" ) == 0 )
					{
						a->acl_dn_style = ACL_STYLE_ONE;
						ber_str2bv( right, 0, 1, &a->acl_dn_pat );

					} else if ( strcasecmp( style, "subtree" ) == 0 ||
						strcasecmp( style, "sub" ) == 0 )
					{
						if( *right == '\0' ) {
							ber_str2bv( "*", STRLENOF( "*" ), 1, &a->acl_dn_pat );

						} else {
							a->acl_dn_style = ACL_STYLE_SUBTREE;
							ber_str2bv( right, 0, 1, &a->acl_dn_pat );
						}

					} else if ( strcasecmp( style, "children" ) == 0 ) {
						a->acl_dn_style = ACL_STYLE_CHILDREN;
						ber_str2bv( right, 0, 1, &a->acl_dn_pat );

					} else if ( strcasecmp( style, "regex" ) == 0 ) {
						a->acl_dn_style = ACL_STYLE_REGEX;

						if ( *right == '\0' ) {
							/* empty regex should match empty DN */
							a->acl_dn_style = ACL_STYLE_BASE;
							ber_str2bv( right, 0, 1, &a->acl_dn_pat );

						} else if ( strcmp(right, "*") == 0 
							|| strcmp(right, ".*") == 0 
							|| strcmp(right, ".*$") == 0 
							|| strcmp(right, "^.*") == 0 
							|| strcmp(right, "^.*$") == 0
							|| strcmp(right, ".*$$") == 0 
							|| strcmp(right, "^.*$$") == 0 )
						{
							ber_str2bv( "*", STRLENOF("*"), 1, &a->acl_dn_pat );

						} else {
							acl_regex_normalized_dn( right, &a->acl_dn_pat );
						}

					} else {
						fprintf( stderr, "%s: line %d: "
							"unknown dn style \"%s\" in to clause\n",
						    fname, lineno, style );
						acl_usage();
					}

					continue;
				}

				if ( strcasecmp( left, "filter" ) == 0 ) {
					if ( (a->acl_filter = str2filter( right )) == NULL ) {
						fprintf( stderr,
				"%s: line %d: bad filter \"%s\" in to clause\n",
						    fname, lineno, right );
						acl_usage();
					}

				} else if ( strcasecmp( left, "attr" ) == 0		/* TOLERATED */
						|| strcasecmp( left, "attrs" ) == 0 )	/* DOCUMENTED */
				{
					a->acl_attrs = str2anlist( a->acl_attrs,
						right, "," );
					if ( a->acl_attrs == NULL ) {
						fprintf( stderr,
				"%s: line %d: unknown attr \"%s\" in to clause\n",
						    fname, lineno, right );
						acl_usage();
					}

				} else if ( strncasecmp( left, "val", 3 ) == 0 ) {
					char	*mr;
					
					if ( !BER_BVISEMPTY( &a->acl_attrval ) ) {
						fprintf( stderr,
				"%s: line %d: attr val already specified in to clause.\n",
							fname, lineno );
						acl_usage();
					}
					if ( a->acl_attrs == NULL || !BER_BVISEMPTY( &a->acl_attrs[1].an_name ) )
					{
						fprintf( stderr,
				"%s: line %d: attr val requires a single attribute.\n",
							fname, lineno );
						acl_usage();
					}

					ber_str2bv( right, 0, 1, &a->acl_attrval );
					a->acl_attrval_style = ACL_STYLE_BASE;

					mr = strchr( left, '/' );
					if ( mr != NULL ) {
						mr[ 0 ] = '\0';
						mr++;

						a->acl_attrval_mr = mr_find( mr );
						if ( a->acl_attrval_mr == NULL ) {
							fprintf( stderr, "%s: line %d: "
								"invalid matching rule \"%s\".\n",
								fname, lineno, mr );
							acl_usage();
						}

						if( !mr_usable_with_at( a->acl_attrval_mr, a->acl_attrs[ 0 ].an_desc->ad_type ) )
						{
							fprintf( stderr, "%s: line %d: "
								"matching rule \"%s\" use "
								"with attr \"%s\" not appropriate.\n",
								fname, lineno, mr,
								a->acl_attrs[ 0 ].an_name.bv_val );
							acl_usage();
						}
					}
					
					if ( style != NULL ) {
						if ( strcasecmp( style, "regex" ) == 0 ) {
							int e = regcomp( &a->acl_attrval_re, a->acl_attrval.bv_val,
								REG_EXTENDED | REG_ICASE | REG_NOSUB );
							if ( e ) {
								char buf[512];
								regerror( e, &a->acl_attrval_re, buf, sizeof(buf) );
								fprintf( stderr, "%s: line %d: "
									"regular expression \"%s\" bad because of %s\n",
									fname, lineno, right, buf );
								acl_usage();
							}
							a->acl_attrval_style = ACL_STYLE_REGEX;

						} else {
							/* FIXME: if the attribute has DN syntax, we might
							 * allow one, subtree and children styles as well */
							if ( !strcasecmp( style, "base" ) ||
								!strcasecmp( style, "exact" ) ) {
								a->acl_attrval_style = ACL_STYLE_BASE;

							} else if ( a->acl_attrs[0].an_desc->ad_type->
								sat_syntax == slap_schema.si_syn_distinguishedName )
							{
								struct berval	bv;

								if ( !strcasecmp( style, "baseObject" ) ||
									!strcasecmp( style, "base" ) )
								{
									a->acl_attrval_style = ACL_STYLE_BASE;
								} else if ( !strcasecmp( style, "onelevel" ) ||
									!strcasecmp( style, "one" ) )
								{
									a->acl_attrval_style = ACL_STYLE_ONE;
								} else if ( !strcasecmp( style, "subtree" ) ||
									!strcasecmp( style, "sub" ) )
								{
									a->acl_attrval_style = ACL_STYLE_SUBTREE;
								} else if ( !strcasecmp( style, "children" ) ) {
									a->acl_attrval_style = ACL_STYLE_CHILDREN;
								} else {
									fprintf( stderr, 
										"%s: line %d: unknown val.<style> \"%s\" "
										"for attributeType \"%s\" with DN syntax; "
										"using \"base\"\n",
										fname, lineno, style,
										a->acl_attrs[0].an_desc->ad_cname.bv_val );
									a->acl_attrval_style = ACL_STYLE_BASE;
								}

								bv = a->acl_attrval;
								rc = dnNormalize( 0, NULL, NULL, &bv, &a->acl_attrval, NULL );
								if ( rc != LDAP_SUCCESS ) {
									fprintf( stderr, 
										"%s: line %d: unable to normalize DN \"%s\" "
										"for attributeType \"%s\" (%d).\n",
										fname, lineno, bv.bv_val,
										a->acl_attrs[0].an_desc->ad_cname.bv_val, rc );
									acl_usage();
								}
								ber_memfree( bv.bv_val );

							} else {
								fprintf( stderr, 
									"%s: line %d: unknown val.<style> \"%s\" "
									"for attributeType \"%s\"; using \"exact\"\n",
									fname, lineno, style,
									a->acl_attrs[0].an_desc->ad_cname.bv_val );
								a->acl_attrval_style = ACL_STYLE_BASE;
							}
						}
					}

					/* Check for appropriate matching rule */
					if ( a->acl_attrval_style != ACL_STYLE_REGEX ) {
						if ( a->acl_attrval_mr == NULL ) {
							a->acl_attrval_mr = a->acl_attrs[ 0 ].an_desc->ad_type->sat_equality;
						}

						if ( a->acl_attrval_mr == NULL ) {
							fprintf( stderr, "%s: line %d: "
								"attr \"%s\" must have an EQUALITY matching rule.\n",
								fname, lineno, a->acl_attrs[ 0 ].an_name.bv_val );
							acl_usage();
						}
					}

				} else {
					fprintf( stderr,
						"%s: line %d: expecting <what> got \"%s\"\n",
					    fname, lineno, left );
					acl_usage();
				}
			}

			if ( !BER_BVISNULL( &a->acl_dn_pat ) && 
					ber_bvccmp( &a->acl_dn_pat, '*' ) )
			{
				free( a->acl_dn_pat.bv_val );
				BER_BVZERO( &a->acl_dn_pat );
				a->acl_dn_style = ACL_STYLE_REGEX;
			}
			
			if ( !BER_BVISEMPTY( &a->acl_dn_pat ) ||
					a->acl_dn_style != ACL_STYLE_REGEX ) 
			{
				if ( a->acl_dn_style != ACL_STYLE_REGEX ) {
					struct berval bv;
					rc = dnNormalize( 0, NULL, NULL, &a->acl_dn_pat, &bv, NULL);
					if ( rc != LDAP_SUCCESS ) {
						fprintf( stderr,
							"%s: line %d: bad DN \"%s\" in to DN clause\n",
							fname, lineno, a->acl_dn_pat.bv_val );
						acl_usage();
					}
					free( a->acl_dn_pat.bv_val );
					a->acl_dn_pat = bv;

				} else {
					int e = regcomp( &a->acl_dn_re, a->acl_dn_pat.bv_val,
						REG_EXTENDED | REG_ICASE );
					if ( e ) {
						char buf[512];
						regerror( e, &a->acl_dn_re, buf, sizeof(buf) );
						fprintf( stderr, "%s: line %d: "
							"regular expression \"%s\" bad because of %s\n",
							fname, lineno, right, buf );
						acl_usage();
					}
				}
			}

		/* by clause - select who has what access to entries */
		} else if ( strcasecmp( argv[i], "by" ) == 0 ) {
			if ( a == NULL ) {
				fprintf( stderr, "%s: line %d: "
					"to clause required before by clause in access line\n",
				    fname, lineno );
				acl_usage();
			}

			/*
			 * by clause consists of <who> and <access>
			 */

			b = (Access *) ch_calloc( 1, sizeof(Access) );

			ACL_INVALIDATE( b->a_access_mask );

			if ( ++i == argc ) {
				fprintf( stderr,
			    "%s: line %d: premature eol: expecting <who>\n",
				    fname, lineno );
				acl_usage();
			}

			/* get <who> */
			for ( ; i < argc; i++ ) {
				slap_style_t	sty = ACL_STYLE_REGEX;
				char		*style_modifier = NULL;
				char		*style_level = NULL;
				int		level = 0;
				int		expand = 0;
				slap_dn_access	*bdn = &b->a_dn;
				int		is_realdn = 0;

				split( argv[i], '=', &left, &right );
				split( left, '.', &left, &style );
				if ( style ) {
					split( style, ',', &style, &style_modifier );

					if ( strncasecmp( style, "level", STRLENOF( "level" ) ) == 0 ) {
						split( style, '{', &style, &style_level );
						if ( style_level != NULL ) {
							char *p = strchr( style_level, '}' );
							if ( p == NULL ) {
								fprintf( stderr,
									"%s: line %d: premature eol: "
									"expecting closing '}' in \"level{n}\"\n",
									fname, lineno );
								acl_usage();
							} else if ( p == style_level ) {
								fprintf( stderr,
									"%s: line %d: empty level "
									"in \"level{n}\"\n",
									fname, lineno );
								acl_usage();
							}
							p[0] = '\0';
						}
					}
				}

				if ( style == NULL || *style == '\0' ||
					strcasecmp( style, "exact" ) == 0 ||
					strcasecmp( style, "baseObject" ) == 0 ||
					strcasecmp( style, "base" ) == 0 )
				{
					sty = ACL_STYLE_BASE;

				} else if ( strcasecmp( style, "onelevel" ) == 0 ||
					strcasecmp( style, "one" ) == 0 )
				{
					sty = ACL_STYLE_ONE;

				} else if ( strcasecmp( style, "subtree" ) == 0 ||
					strcasecmp( style, "sub" ) == 0 )
				{
					sty = ACL_STYLE_SUBTREE;

				} else if ( strcasecmp( style, "children" ) == 0 ) {
					sty = ACL_STYLE_CHILDREN;

				} else if ( strcasecmp( style, "level" ) == 0 )
				{
					char	*next;

					level = strtol( style_level, &next, 10 );
					if ( next[0] != '\0' ) {
						fprintf( stderr,
							"%s: line %d: unable to parse level "
							"in \"level{n}\"\n",
							fname, lineno );
						acl_usage();
					}

					sty = ACL_STYLE_LEVEL;

				} else if ( strcasecmp( style, "regex" ) == 0 ) {
					sty = ACL_STYLE_REGEX;

				} else if ( strcasecmp( style, "expand" ) == 0 ) {
					sty = ACL_STYLE_EXPAND;

				} else if ( strcasecmp( style, "ip" ) == 0 ) {
					sty = ACL_STYLE_IP;

				} else if ( strcasecmp( style, "path" ) == 0 ) {
					sty = ACL_STYLE_PATH;
#ifndef LDAP_PF_LOCAL
					fprintf( stderr, "%s: line %d: "
						"path style modifier is useless without local\n",
						fname, lineno );
#endif /* LDAP_PF_LOCAL */

				} else {
					fprintf( stderr,
						"%s: line %d: unknown style \"%s\" in by clause\n",
					    fname, lineno, style );
					acl_usage();
				}

				if ( style_modifier &&
					strcasecmp( style_modifier, "expand" ) == 0 )
				{
					switch ( sty ) {
					case ACL_STYLE_REGEX:
						fprintf( stderr, "%s: line %d: "
							"\"regex\" style implies "
							"\"expand\" modifier" 
							SLAPD_CONF_UNKNOWN_IGNORED ".\n",
							fname, lineno );
#ifdef SLAPD_CONF_UNKNOWN_BAILOUT
						acl_usage();
#endif /* SLAPD_CONF_UNKNOWN_BAILOUT */
						break;

					case ACL_STYLE_EXPAND:
#if 0
						/* FIXME: now it's legal... */
						fprintf( stderr, "%s: line %d: "
							"\"expand\" style used "
							"in conjunction with "
							"\"expand\" modifier"
							SLAPD_CONF_UNKNOWN_IGNORED ".\n",
							fname, lineno );
#ifdef SLAPD_CONF_UNKNOWN_BAILOUT
						acl_usage();
#endif /* SLAPD_CONF_UNKNOWN_BAILOUT */
#endif
						break;

					default:
						/* we'll see later if it's pertinent */
						expand = 1;
						break;
					}
				}

				/* expand in <who> needs regex in <what> */
				if ( ( sty == ACL_STYLE_EXPAND || expand )
						&& a->acl_dn_style != ACL_STYLE_REGEX )
				{
					fprintf( stderr, "%s: line %d: "
						"\"expand\" style or modifier used "
						"in conjunction with "
						"a non-regex <what> clause\n",
						fname, lineno );
				}

				if ( strncasecmp( left, "real", STRLENOF( "real" ) ) == 0 ) {
					is_realdn = 1;
					bdn = &b->a_realdn;
					left += STRLENOF( "real" );
				}

				if ( strcasecmp( left, "*" ) == 0 ) {
					if ( is_realdn ) {
						acl_usage();
					}

					ber_str2bv( "*", STRLENOF( "*" ), 1, &bv );
					sty = ACL_STYLE_REGEX;

				} else if ( strcasecmp( left, "anonymous" ) == 0 ) {
					ber_str2bv("anonymous", STRLENOF( "anonymous" ), 1, &bv);
					sty = ACL_STYLE_ANONYMOUS;

				} else if ( strcasecmp( left, "users" ) == 0 ) {
					ber_str2bv("users", STRLENOF( "users" ), 1, &bv);
					sty = ACL_STYLE_USERS;

				} else if ( strcasecmp( left, "self" ) == 0 ) {
					ber_str2bv("self", STRLENOF( "self" ), 1, &bv);
					sty = ACL_STYLE_SELF;

				} else if ( strcasecmp( left, "dn" ) == 0 ) {
					if ( sty == ACL_STYLE_REGEX ) {
						bdn->a_style = ACL_STYLE_REGEX;
						if ( right == NULL ) {
							/* no '=' */
							ber_str2bv("users",
								STRLENOF( "users" ),
								1, &bv);
							bdn->a_style = ACL_STYLE_USERS;

						} else if (*right == '\0' ) {
							/* dn="" */
							ber_str2bv("anonymous",
								STRLENOF( "anonymous" ),
								1, &bv);
							bdn->a_style = ACL_STYLE_ANONYMOUS;

						} else if ( strcmp( right, "*" ) == 0 ) {
							/* dn=* */
							/* any or users?  users for now */
							ber_str2bv("users",
								STRLENOF( "users" ),
								1, &bv);
							bdn->a_style = ACL_STYLE_USERS;

						} else if ( strcmp( right, ".+" ) == 0
							|| strcmp( right, "^.+" ) == 0
							|| strcmp( right, ".+$" ) == 0
							|| strcmp( right, "^.+$" ) == 0
							|| strcmp( right, ".+$$" ) == 0
							|| strcmp( right, "^.+$$" ) == 0 )
						{
							ber_str2bv("users",
								STRLENOF( "users" ),
								1, &bv);
							bdn->a_style = ACL_STYLE_USERS;

						} else if ( strcmp( right, ".*" ) == 0
							|| strcmp( right, "^.*" ) == 0
							|| strcmp( right, ".*$" ) == 0
							|| strcmp( right, "^.*$" ) == 0
							|| strcmp( right, ".*$$" ) == 0
							|| strcmp( right, "^.*$$" ) == 0 )
						{
							ber_str2bv("*",
								STRLENOF( "*" ),
								1, &bv);

						} else {
							acl_regex_normalized_dn( right, &bv );
							if ( !ber_bvccmp( &bv, '*' ) ) {
								regtest( fname, lineno, bv.bv_val );
							}
						}

					} else if ( right == NULL || *right == '\0' ) {
						fprintf( stderr, "%s: line %d: "
							"missing \"=\" in (or value after) \"%s\" "
							"in by clause\n",
						    fname, lineno, left );
						acl_usage();

					} else {
						ber_str2bv( right, 0, 1, &bv );
					}

				} else {
					BER_BVZERO( &bv );
				}

				if ( !BER_BVISNULL( &bv ) ) {
					if ( !BER_BVISEMPTY( &bdn->a_pat ) ) {
						fprintf( stderr,
						    "%s: line %d: dn pattern already specified.\n",
						    fname, lineno );
						acl_usage();
					}

					if ( sty != ACL_STYLE_REGEX &&
							sty != ACL_STYLE_ANONYMOUS &&
							sty != ACL_STYLE_USERS &&
							sty != ACL_STYLE_SELF &&
							expand == 0 )
					{
						rc = dnNormalize(0, NULL, NULL,
							&bv, &bdn->a_pat, NULL);
						if ( rc != LDAP_SUCCESS ) {
							fprintf( stderr,
								"%s: line %d: bad DN \"%s\" in by DN clause\n",
								fname, lineno, bv.bv_val );
							acl_usage();
						}
						free( bv.bv_val );

					} else {
						bdn->a_pat = bv;
					}
					bdn->a_style = sty;
					if ( expand ) {
						char	*exp;
						int	gotit = 0;

						for ( exp = strchr( bdn->a_pat.bv_val, '$' );
								exp && exp - bdn->a_pat.bv_val < bdn->a_pat.bv_len;
								exp = strchr( exp, '$' ) )
						{
							if ( isdigit( exp[ 1 ] ) ) {
								gotit = 1;
								break;
							}
						}

						if ( gotit == 1 ) {
							bdn->a_expand = expand;

						} else {
							fprintf( stderr,
								"%s: line %d: \"expand\" used "
								"with no expansions in \"pattern\""
								SLAPD_CONF_UNKNOWN_IGNORED ".\n",
								fname, lineno );
#ifdef SLAPD_CONF_UNKNOWN_BAILOUT
							acl_usage();
#endif /* SLAPD_CONF_UNKNOWN_BAILOUT */
						} 
					}
					if ( sty == ACL_STYLE_SELF ) {
						bdn->a_self_level = level;

					} else {
						if ( level < 0 ) {
							fprintf( stderr,
								"%s: line %d: bad negative level \"%d\" "
								"in by DN clause\n",
								fname, lineno, level );
							acl_usage();
						} else if ( level == 1 ) {
							fprintf( stderr,
								"%s: line %d: \"onelevel\" should be used "
								"instead of \"level{1}\" in by DN clause\n",
								fname, lineno );
						} else if ( level == 0 && sty == ACL_STYLE_LEVEL ) {
							fprintf( stderr,
								"%s: line %d: \"base\" should be used "
								"instead of \"level{0}\" in by DN clause\n",
								fname, lineno );
						}

						bdn->a_level = level;
					}
					continue;
				}

				if ( strcasecmp( left, "dnattr" ) == 0 ) {
					if ( right == NULL || right[0] == '\0' ) {
						fprintf( stderr, "%s: line %d: "
							"missing \"=\" in (or value after) \"%s\" "
							"in by clause\n",
							fname, lineno, left );
						acl_usage();
					}

					if( bdn->a_at != NULL ) {
						fprintf( stderr,
							"%s: line %d: dnattr already specified.\n",
							fname, lineno );
						acl_usage();
					}

					rc = slap_str2ad( right, &bdn->a_at, &text );

					if( rc != LDAP_SUCCESS ) {
						fprintf( stderr,
							"%s: line %d: dnattr \"%s\": %s\n",
							fname, lineno, right, text );
						acl_usage();
					}


					if( !is_at_syntax( bdn->a_at->ad_type,
						SLAPD_DN_SYNTAX ) &&
						!is_at_syntax( bdn->a_at->ad_type,
						SLAPD_NAMEUID_SYNTAX ))
					{
						fprintf( stderr,
							"%s: line %d: dnattr \"%s\": "
							"inappropriate syntax: %s\n",
							fname, lineno, right,
							bdn->a_at->ad_type->sat_syntax_oid );
						acl_usage();
					}

					if( bdn->a_at->ad_type->sat_equality == NULL ) {
						fprintf( stderr,
							"%s: line %d: dnattr \"%s\": "
							"inappropriate matching (no EQUALITY)\n",
							fname, lineno, right );
						acl_usage();
					}

					continue;
				}

				if ( strncasecmp( left, "group", STRLENOF( "group" ) ) == 0 ) {
					char *name = NULL;
					char *value = NULL;

					switch ( sty ) {
					case ACL_STYLE_REGEX:
						/* legacy, tolerated */
						fprintf( stderr, "%s: line %d: "
							"deprecated group style \"regex\"; "
							"use \"expand\" instead\n",
							fname, lineno );
						sty = ACL_STYLE_EXPAND;
						break;

					case ACL_STYLE_BASE:
						/* legal, traditional */
					case ACL_STYLE_EXPAND:
						/* legal, substring expansion; supersedes regex */
						break;

					default:
						/* unknown */
						fprintf( stderr, "%s: line %d: "
							"inappropriate style \"%s\" in by clause\n",
							fname, lineno, style );
						acl_usage();
					}

					if ( right == NULL || right[0] == '\0' ) {
						fprintf( stderr, "%s: line %d: "
							"missing \"=\" in (or value after) \"%s\" "
							"in by clause\n",
							fname, lineno, left );
						acl_usage();
					}

					if ( !BER_BVISEMPTY( &b->a_group_pat ) ) {
						fprintf( stderr,
							"%s: line %d: group pattern already specified.\n",
							fname, lineno );
						acl_usage();
					}

					/* format of string is
						"group/objectClassValue/groupAttrName" */
					if ( ( value = strchr(left, '/') ) != NULL ) {
						*value++ = '\0';
						if ( *value && ( name = strchr( value, '/' ) ) != NULL ) {
							*name++ = '\0';
						}
					}

					b->a_group_style = sty;
					if ( sty == ACL_STYLE_EXPAND ) {
						acl_regex_normalized_dn( right, &bv );
						if ( !ber_bvccmp( &bv, '*' ) ) {
							regtest( fname, lineno, bv.bv_val );
						}
						b->a_group_pat = bv;

					} else {
						ber_str2bv( right, 0, 0, &bv );
						rc = dnNormalize( 0, NULL, NULL, &bv,
							&b->a_group_pat, NULL );
						if ( rc != LDAP_SUCCESS ) {
							fprintf( stderr,
								"%s: line %d: bad DN \"%s\"\n",
								fname, lineno, right );
							acl_usage();
						}
					}

					if ( value && *value ) {
						b->a_group_oc = oc_find( value );
						*--value = '/';

						if ( b->a_group_oc == NULL ) {
							fprintf( stderr,
								"%s: line %d: group objectclass "
								"\"%s\" unknown\n",
								fname, lineno, value );
							acl_usage();
						}

					} else {
						b->a_group_oc = oc_find( SLAPD_GROUP_CLASS );

						if( b->a_group_oc == NULL ) {
							fprintf( stderr,
								"%s: line %d: group default objectclass "
								"\"%s\" unknown\n",
								fname, lineno, SLAPD_GROUP_CLASS );
							acl_usage();
						}
					}

					if ( is_object_subclass( slap_schema.si_oc_referral,
						b->a_group_oc ) )
					{
						fprintf( stderr,
							"%s: line %d: group objectclass \"%s\" "
							"is subclass of referral\n",
							fname, lineno, value );
						acl_usage();
					}

					if ( is_object_subclass( slap_schema.si_oc_alias,
						b->a_group_oc ) )
					{
						fprintf( stderr,
							"%s: line %d: group objectclass \"%s\" "
							"is subclass of alias\n",
							fname, lineno, value );
						acl_usage();
					}

					if ( name && *name ) {
						rc = slap_str2ad( name, &b->a_group_at, &text );

						if( rc != LDAP_SUCCESS ) {
							fprintf( stderr,
								"%s: line %d: group \"%s\": %s\n",
								fname, lineno, right, text );
							acl_usage();
						}
						*--name = '/';

					} else {
						rc = slap_str2ad( SLAPD_GROUP_ATTR, &b->a_group_at, &text );

						if ( rc != LDAP_SUCCESS ) {
							fprintf( stderr,
								"%s: line %d: group \"%s\": %s\n",
								fname, lineno, SLAPD_GROUP_ATTR, text );
							acl_usage();
						}
					}

					if ( !is_at_syntax( b->a_group_at->ad_type,
						SLAPD_DN_SYNTAX ) &&
						!is_at_syntax( b->a_group_at->ad_type,
						SLAPD_NAMEUID_SYNTAX ) &&
						!is_at_subtype( b->a_group_at->ad_type, slap_schema.si_ad_labeledURI->ad_type ) )
					{
						fprintf( stderr,
							"%s: line %d: group \"%s\": inappropriate syntax: %s\n",
							fname, lineno, right,
							b->a_group_at->ad_type->sat_syntax_oid );
						acl_usage();
					}


					{
						int rc;
						struct berval vals[2];

						ber_str2bv( b->a_group_oc->soc_oid, 0, 0, &vals[0] );
						BER_BVZERO( &vals[1] );

						rc = oc_check_allowed( b->a_group_at->ad_type,
							vals, NULL );

						if( rc != 0 ) {
							fprintf( stderr, "%s: line %d: "
								"group: \"%s\" not allowed by \"%s\"\n",
								fname, lineno,
								b->a_group_at->ad_cname.bv_val,
								b->a_group_oc->soc_oid );
							acl_usage();
						}
					}
					continue;
				}

				if ( strcasecmp( left, "peername" ) == 0 ) {
					switch ( sty ) {
					case ACL_STYLE_REGEX:
					case ACL_STYLE_BASE:
						/* legal, traditional */
					case ACL_STYLE_EXPAND:
						/* cheap replacement to regex for simple expansion */
					case ACL_STYLE_IP:
					case ACL_STYLE_PATH:
						/* legal, peername specific */
						break;

					default:
						fprintf( stderr, "%s: line %d: "
							"inappropriate style \"%s\" in by clause\n",
						    fname, lineno, style );
						acl_usage();
					}

					if ( right == NULL || right[0] == '\0' ) {
						fprintf( stderr, "%s: line %d: "
							"missing \"=\" in (or value after) \"%s\" "
							"in by clause\n",
							fname, lineno, left );
						acl_usage();
					}

					if ( !BER_BVISEMPTY( &b->a_peername_pat ) ) {
						fprintf( stderr, "%s: line %d: "
							"peername pattern already specified.\n",
							fname, lineno );
						acl_usage();
					}

					b->a_peername_style = sty;
					if ( sty == ACL_STYLE_REGEX ) {
						acl_regex_normalized_dn( right, &bv );
						if ( !ber_bvccmp( &bv, '*' ) ) {
							regtest( fname, lineno, bv.bv_val );
						}
						b->a_peername_pat = bv;

					} else {
						ber_str2bv( right, 0, 1, &b->a_peername_pat );

						if ( sty == ACL_STYLE_IP ) {
							char		*addr = NULL,
									*mask = NULL,
									*port = NULL;

							split( right, '{', &addr, &port );
							split( addr, '%', &addr, &mask );

							b->a_peername_addr = inet_addr( addr );
							if ( b->a_peername_addr == (unsigned long)(-1) ) {
								/* illegal address */
								fprintf( stderr, "%s: line %d: "
									"illegal peername address \"%s\".\n",
									fname, lineno, addr );
								acl_usage();
							}

							b->a_peername_mask = (unsigned long)(-1);
							if ( mask != NULL ) {
								b->a_peername_mask = inet_addr( mask );
								if ( b->a_peername_mask ==
									(unsigned long)(-1) )
								{
									/* illegal mask */
									fprintf( stderr, "%s: line %d: "
										"illegal peername address mask "
										"\"%s\".\n",
										fname, lineno, mask );
									acl_usage();
								}
							} 

							b->a_peername_port = -1;
							if ( port ) {
								char	*end = NULL;

								b->a_peername_port = strtol( port, &end, 10 );
								if ( end[0] != '}' ) {
									/* illegal port */
									fprintf( stderr, "%s: line %d: "
										"illegal peername port specification "
										"\"{%s}\".\n",
										fname, lineno, port );
									acl_usage();
								}
							}
						}
					}
					continue;
				}

				if ( strcasecmp( left, "sockname" ) == 0 ) {
					switch ( sty ) {
					case ACL_STYLE_REGEX:
					case ACL_STYLE_BASE:
						/* legal, traditional */
					case ACL_STYLE_EXPAND:
						/* cheap replacement to regex for simple expansion */
						break;

					default:
						/* unknown */
						fprintf( stderr, "%s: line %d: "
							"inappropriate style \"%s\" in by clause\n",
						    fname, lineno, style );
						acl_usage();
					}

					if ( right == NULL || right[0] == '\0' ) {
						fprintf( stderr, "%s: line %d: "
							"missing \"=\" in (or value after) \"%s\" "
							"in by clause\n",
							fname, lineno, left );
						acl_usage();
					}

					if ( !BER_BVISNULL( &b->a_sockname_pat ) ) {
						fprintf( stderr, "%s: line %d: "
							"sockname pattern already specified.\n",
							fname, lineno );
						acl_usage();
					}

					b->a_sockname_style = sty;
					if ( sty == ACL_STYLE_REGEX ) {
						acl_regex_normalized_dn( right, &bv );
						if ( !ber_bvccmp( &bv, '*' ) ) {
							regtest( fname, lineno, bv.bv_val );
						}
						b->a_sockname_pat = bv;
						
					} else {
						ber_str2bv( right, 0, 1, &b->a_sockname_pat );
					}
					continue;
				}

				if ( strcasecmp( left, "domain" ) == 0 ) {
					switch ( sty ) {
					case ACL_STYLE_REGEX:
					case ACL_STYLE_BASE:
					case ACL_STYLE_SUBTREE:
						/* legal, traditional */
						break;

					case ACL_STYLE_EXPAND:
						/* tolerated: means exact,expand */
						if ( expand ) {
							fprintf( stderr,
								"%s: line %d: "
								"\"expand\" modifier "
								"with \"expand\" style\n",
								fname, lineno );
						}
						sty = ACL_STYLE_BASE;
						expand = 1;
						break;

					default:
						/* unknown */
						fprintf( stderr, "%s: line %d: "
							"inappropriate style \"%s\" in by clause\n",
						    fname, lineno, style );
						acl_usage();
					}

					if ( right == NULL || right[0] == '\0' ) {
						fprintf( stderr, "%s: line %d: "
							"missing \"=\" in (or value after) \"%s\" "
							"in by clause\n",
							fname, lineno, left );
						acl_usage();
					}

					if ( !BER_BVISEMPTY( &b->a_domain_pat ) ) {
						fprintf( stderr,
							"%s: line %d: domain pattern already specified.\n",
							fname, lineno );
						acl_usage();
					}

					b->a_domain_style = sty;
					b->a_domain_expand = expand;
					if ( sty == ACL_STYLE_REGEX ) {
						acl_regex_normalized_dn( right, &bv );
						if ( !ber_bvccmp( &bv, '*' ) ) {
							regtest( fname, lineno, bv.bv_val );
						}
						b->a_domain_pat = bv;

					} else {
						ber_str2bv( right, 0, 1, &b->a_domain_pat );
					}
					continue;
				}

				if ( strcasecmp( left, "sockurl" ) == 0 ) {
					switch ( sty ) {
					case ACL_STYLE_REGEX:
					case ACL_STYLE_BASE:
						/* legal, traditional */
					case ACL_STYLE_EXPAND:
						/* cheap replacement to regex for simple expansion */
						break;

					default:
						/* unknown */
						fprintf( stderr, "%s: line %d: "
							"inappropriate style \"%s\" in by clause\n",
						    fname, lineno, style );
						acl_usage();
					}

					if ( right == NULL || right[0] == '\0' ) {
						fprintf( stderr, "%s: line %d: "
							"missing \"=\" in (or value after) \"%s\" "
							"in by clause\n",
							fname, lineno, left );
						acl_usage();
					}

					if ( !BER_BVISEMPTY( &b->a_sockurl_pat ) ) {
						fprintf( stderr,
							"%s: line %d: sockurl pattern already specified.\n",
							fname, lineno );
						acl_usage();
					}

					b->a_sockurl_style = sty;
					if ( sty == ACL_STYLE_REGEX ) {
						acl_regex_normalized_dn( right, &bv );
						if ( !ber_bvccmp( &bv, '*' ) ) {
							regtest( fname, lineno, bv.bv_val );
						}
						b->a_sockurl_pat = bv;
						
					} else {
						ber_str2bv( right, 0, 1, &b->a_sockurl_pat );
					}
					continue;
				}

				if ( strcasecmp( left, "set" ) == 0 ) {
					switch ( sty ) {
						/* deprecated */
					case ACL_STYLE_REGEX:
						fprintf( stderr, "%s: line %d: "
							"deprecated set style "
							"\"regex\" in <by> clause; "
							"use \"expand\" instead\n",
							fname, lineno );
						sty = ACL_STYLE_EXPAND;
						/* FALLTHRU */
						
					case ACL_STYLE_BASE:
					case ACL_STYLE_EXPAND:
						break;

					default:
						fprintf( stderr, "%s: line %d: "
							"inappropriate style \"%s\" in by clause\n",
							fname, lineno, style );
						acl_usage();
					}

					if ( !BER_BVISEMPTY( &b->a_set_pat ) ) {
						fprintf( stderr,
							"%s: line %d: set attribute already specified.\n",
							fname, lineno );
						acl_usage();
					}

					if ( right == NULL || *right == '\0' ) {
						fprintf( stderr,
							"%s: line %d: no set is defined\n",
							fname, lineno );
						acl_usage();
					}

					b->a_set_style = sty;
					ber_str2bv( right, 0, 1, &b->a_set_pat );

					continue;
				}

#ifdef SLAP_DYNACL
				{
					char		*name = NULL;
					
					if ( strcasecmp( left, "aci" ) == 0 ) {
						name = "aci";
						
					} else if ( strncasecmp( left, "dynacl/", STRLENOF( "dynacl/" ) ) == 0 ) {
						name = &left[ STRLENOF( "dynacl/" ) ];
					}

					if ( name ) {
						if ( slap_dynacl_config( fname, lineno, b, name, sty, right ) ) {
							fprintf( stderr, "%s: line %d: "
								"unable to configure dynacl \"%s\"\n",
								fname, lineno, name );
							acl_usage();
						}

						continue;
					}
				}
#else /* ! SLAP_DYNACL */

#ifdef SLAPD_ACI_ENABLED
				if ( strcasecmp( left, "aci" ) == 0 ) {
					if (sty != ACL_STYLE_REGEX && sty != ACL_STYLE_BASE) {
						fprintf( stderr, "%s: line %d: "
							"inappropriate style \"%s\" in by clause\n",
						    fname, lineno, style );
						acl_usage();
					}

					if( b->a_aci_at != NULL ) {
						fprintf( stderr,
							"%s: line %d: aci attribute already specified.\n",
							fname, lineno );
						acl_usage();
					}

					if ( right != NULL && *right != '\0' ) {
						rc = slap_str2ad( right, &b->a_aci_at, &text );

						if( rc != LDAP_SUCCESS ) {
							fprintf( stderr,
								"%s: line %d: aci \"%s\": %s\n",
								fname, lineno, right, text );
							acl_usage();
						}

					} else {
						b->a_aci_at = slap_schema.si_ad_aci;
					}

					if( !is_at_syntax( b->a_aci_at->ad_type,
						SLAPD_ACI_SYNTAX) )
					{
						fprintf( stderr, "%s: line %d: "
							"aci \"%s\": inappropriate syntax: %s\n",
							fname, lineno, right,
							b->a_aci_at->ad_type->sat_syntax_oid );
						acl_usage();
					}

					continue;
				}
#endif /* SLAPD_ACI_ENABLED */
#endif /* ! SLAP_DYNACL */

				if ( strcasecmp( left, "ssf" ) == 0 ) {
					if ( sty != ACL_STYLE_REGEX && sty != ACL_STYLE_BASE ) {
						fprintf( stderr, "%s: line %d: "
							"inappropriate style \"%s\" in by clause\n",
						    fname, lineno, style );
						acl_usage();
					}

					if ( b->a_authz.sai_ssf ) {
						fprintf( stderr,
							"%s: line %d: ssf attribute already specified.\n",
							fname, lineno );
						acl_usage();
					}

					if ( right == NULL || *right == '\0' ) {
						fprintf( stderr,
							"%s: line %d: no ssf is defined\n",
							fname, lineno );
						acl_usage();
					}

					b->a_authz.sai_ssf = strtol( right, &next, 10 );
					if ( next == NULL || next[0] != '\0' ) {
						fprintf( stderr,
							"%s: line %d: unable to parse ssf value (%s)\n",
							fname, lineno, right );
						acl_usage();
					}

					if ( !b->a_authz.sai_ssf ) {
						fprintf( stderr,
							"%s: line %d: invalid ssf value (%s)\n",
							fname, lineno, right );
						acl_usage();
					}
					continue;
				}

				if ( strcasecmp( left, "transport_ssf" ) == 0 ) {
					if ( sty != ACL_STYLE_REGEX && sty != ACL_STYLE_BASE ) {
						fprintf( stderr, "%s: line %d: "
							"inappropriate style \"%s\" in by clause\n",
							fname, lineno, style );
						acl_usage();
					}

					if ( b->a_authz.sai_transport_ssf ) {
						fprintf( stderr, "%s: line %d: "
							"transport_ssf attribute already specified.\n",
							fname, lineno );
						acl_usage();
					}

					if ( right == NULL || *right == '\0' ) {
						fprintf( stderr,
							"%s: line %d: no transport_ssf is defined\n",
							fname, lineno );
						acl_usage();
					}

					b->a_authz.sai_transport_ssf = strtol( right, &next, 10 );
					if ( next == NULL || next[0] != '\0' ) {
						fprintf( stderr, "%s: line %d: "
							"unable to parse transport_ssf value (%s)\n",
							fname, lineno, right );
						acl_usage();
					}

					if ( !b->a_authz.sai_transport_ssf ) {
						fprintf( stderr,
							"%s: line %d: invalid transport_ssf value (%s)\n",
							fname, lineno, right );
						acl_usage();
					}
					continue;
				}

				if ( strcasecmp( left, "tls_ssf" ) == 0 ) {
					if ( sty != ACL_STYLE_REGEX && sty != ACL_STYLE_BASE ) {
						fprintf( stderr, "%s: line %d: "
							"inappropriate style \"%s\" in by clause\n",
							fname, lineno, style );
						acl_usage();
					}

					if ( b->a_authz.sai_tls_ssf ) {
						fprintf( stderr, "%s: line %d: "
							"tls_ssf attribute already specified.\n",
							fname, lineno );
						acl_usage();
					}

					if ( right == NULL || *right == '\0' ) {
						fprintf( stderr,
							"%s: line %d: no tls_ssf is defined\n",
							fname, lineno );
						acl_usage();
					}

					b->a_authz.sai_tls_ssf = strtol( right, &next, 10 );
					if ( next == NULL || next[0] != '\0' ) {
						fprintf( stderr, "%s: line %d: "
							"unable to parse tls_ssf value (%s)\n",
							fname, lineno, right );
						acl_usage();
					}

					if ( !b->a_authz.sai_tls_ssf ) {
						fprintf( stderr,
							"%s: line %d: invalid tls_ssf value (%s)\n",
							fname, lineno, right );
						acl_usage();
					}
					continue;
				}

				if ( strcasecmp( left, "sasl_ssf" ) == 0 ) {
					if ( sty != ACL_STYLE_REGEX && sty != ACL_STYLE_BASE ) {
						fprintf( stderr, "%s: line %d: "
							"inappropriate style \"%s\" in by clause\n",
							fname, lineno, style );
						acl_usage();
					}

					if ( b->a_authz.sai_sasl_ssf ) {
						fprintf( stderr, "%s: line %d: "
							"sasl_ssf attribute already specified.\n",
							fname, lineno );
						acl_usage();
					}

					if ( right == NULL || *right == '\0' ) {
						fprintf( stderr,
							"%s: line %d: no sasl_ssf is defined\n",
							fname, lineno );
						acl_usage();
					}

					b->a_authz.sai_sasl_ssf = strtol( right, &next, 10 );
					if ( next == NULL || next[0] != '\0' ) {
						fprintf( stderr, "%s: line %d: "
							"unable to parse sasl_ssf value (%s)\n",
							fname, lineno, right );
						acl_usage();
					}

					if ( !b->a_authz.sai_sasl_ssf ) {
						fprintf( stderr,
							"%s: line %d: invalid sasl_ssf value (%s)\n",
							fname, lineno, right );
						acl_usage();
					}
					continue;
				}

				if ( right != NULL ) {
					/* unsplit */
					right[-1] = '=';
				}
				break;
			}

			if ( i == argc || ( strcasecmp( left, "stop" ) == 0 ) ) { 
				/* out of arguments or plain stop */

				ACL_PRIV_ASSIGN( b->a_access_mask, ACL_PRIV_ADDITIVE );
				b->a_type = ACL_STOP;

				access_append( &a->acl_access, b );
				continue;
			}

			if ( strcasecmp( left, "continue" ) == 0 ) {
				/* plain continue */

				ACL_PRIV_ASSIGN( b->a_access_mask, ACL_PRIV_ADDITIVE );
				b->a_type = ACL_CONTINUE;

				access_append( &a->acl_access, b );
				continue;
			}

			if ( strcasecmp( left, "break" ) == 0 ) {
				/* plain continue */

				ACL_PRIV_ASSIGN(b->a_access_mask, ACL_PRIV_ADDITIVE);
				b->a_type = ACL_BREAK;

				access_append( &a->acl_access, b );
				continue;
			}

			if ( strcasecmp( left, "by" ) == 0 ) {
				/* we've gone too far */
				--i;
				ACL_PRIV_ASSIGN( b->a_access_mask, ACL_PRIV_ADDITIVE );
				b->a_type = ACL_STOP;

				access_append( &a->acl_access, b );
				continue;
			}

			/* get <access> */
			if ( strncasecmp( left, "self", STRLENOF( "self" ) ) == 0 ) {
				b->a_dn_self = 1;
				ACL_PRIV_ASSIGN( b->a_access_mask, str2accessmask( &left[ STRLENOF( "self" ) ] ) );

			} else if ( strncasecmp( left, "realself", STRLENOF( "realself" ) ) == 0 ) {
				b->a_realdn_self = 1;
				ACL_PRIV_ASSIGN( b->a_access_mask, str2accessmask( &left[ STRLENOF( "realself" ) ] ) );

			} else {
				ACL_PRIV_ASSIGN( b->a_access_mask, str2accessmask( left ) );
			}

			if ( ACL_IS_INVALID( b->a_access_mask ) ) {
				fprintf( stderr,
					"%s: line %d: expecting <access> got \"%s\"\n",
					fname, lineno, left );
				acl_usage();
			}

			b->a_type = ACL_STOP;

			if ( ++i == argc ) {
				/* out of arguments or plain stop */
				access_append( &a->acl_access, b );
				continue;
			}

			if ( strcasecmp( argv[i], "continue" ) == 0 ) {
				/* plain continue */
				b->a_type = ACL_CONTINUE;

			} else if ( strcasecmp( argv[i], "break" ) == 0 ) {
				/* plain continue */
				b->a_type = ACL_BREAK;

			} else if ( strcasecmp( argv[i], "stop" ) != 0 ) {
				/* gone to far */
				i--;
			}

			access_append( &a->acl_access, b );

		} else {
			fprintf( stderr,
				"%s: line %d: expecting \"to\" "
				"or \"by\" got \"%s\"\n",
				fname, lineno, argv[i] );
			acl_usage();
		}
	}

	/* if we have no real access clause, complain and do nothing */
	if ( a == NULL ) {
		fprintf( stderr, "%s: line %d: "
			"warning: no access clause(s) specified in access line\n",
			fname, lineno );

	} else {
#ifdef LDAP_DEBUG
		if ( ldap_debug & LDAP_DEBUG_ACL ) {
			print_acl( be, a );
		}
#endif
	
		if ( a->acl_access == NULL ) {
			fprintf( stderr, "%s: line %d: "
				"warning: no by clause(s) specified in access line\n",
				fname, lineno );
		}

		if ( be != NULL ) {
			if ( !BER_BVISNULL( &be->be_nsuffix[ 1 ] ) ) {
				fprintf( stderr, "%s: line %d: warning: "
					"scope checking only applies to single-valued "
					"suffix databases\n",
					fname, lineno );
				/* go ahead, since checking is not authoritative */
			}

			switch ( check_scope( be, a ) ) {
			case ACL_SCOPE_UNKNOWN:
				fprintf( stderr, "%s: line %d: warning: "
					"cannot assess the validity of the ACL scope within "
					"backend naming context\n",
					fname, lineno );
				break;

			case ACL_SCOPE_WARN:
				fprintf( stderr, "%s: line %d: warning: "
					"ACL could be out of scope within backend naming context\n",
					fname, lineno );
				break;

			case ACL_SCOPE_PARTIAL:
				fprintf( stderr, "%s: line %d: warning: "
					"ACL appears to be partially out of scope within "
					"backend naming context\n",
					fname, lineno );
				break;

			case ACL_SCOPE_ERR:
				fprintf( stderr, "%s: line %d: warning: "
					"ACL appears to be out of scope within "
					"backend naming context\n",
					fname, lineno );
				break;

			default:
				break;
			}
			acl_append( &be->be_acl, a, pos );

		} else {
			acl_append( &frontendDB->be_acl, a, pos );
		}
	}
}

char *
accessmask2str( slap_mask_t mask, char *buf, int debug )
{
	int	none = 1;
	char	*ptr = buf;

	assert( buf != NULL );

	if ( ACL_IS_INVALID( mask ) ) {
		return "invalid";
	}

	buf[0] = '\0';

	if ( ACL_IS_LEVEL( mask ) ) {
		if ( ACL_LVL_IS_NONE(mask) ) {
			ptr = lutil_strcopy( ptr, "none" );

		} else if ( ACL_LVL_IS_DISCLOSE(mask) ) {
			ptr = lutil_strcopy( ptr, "disclose" );

		} else if ( ACL_LVL_IS_AUTH(mask) ) {
			ptr = lutil_strcopy( ptr, "auth" );

		} else if ( ACL_LVL_IS_COMPARE(mask) ) {
			ptr = lutil_strcopy( ptr, "compare" );

		} else if ( ACL_LVL_IS_SEARCH(mask) ) {
			ptr = lutil_strcopy( ptr, "search" );

		} else if ( ACL_LVL_IS_READ(mask) ) {
			ptr = lutil_strcopy( ptr, "read" );

		} else if ( ACL_LVL_IS_WRITE(mask) ) {
			ptr = lutil_strcopy( ptr, "write" );

		} else if ( ACL_LVL_IS_WADD(mask) ) {
			ptr = lutil_strcopy( ptr, "add" );

		} else if ( ACL_LVL_IS_WDEL(mask) ) {
			ptr = lutil_strcopy( ptr, "delete" );

		} else if ( ACL_LVL_IS_MANAGE(mask) ) {
			ptr = lutil_strcopy( ptr, "manage" );

		} else {
			ptr = lutil_strcopy( ptr, "unknown" );
		}
		
		if ( !debug ) {
			*ptr = '\0';
			return buf;
		}
		*ptr++ = '(';
	}

	if( ACL_IS_ADDITIVE( mask ) ) {
		*ptr++ = '+';

	} else if( ACL_IS_SUBTRACTIVE( mask ) ) {
		*ptr++ = '-';

	} else {
		*ptr++ = '=';
	}

	if ( ACL_PRIV_ISSET(mask, ACL_PRIV_MANAGE) ) {
		none = 0;
		*ptr++ = 'm';
	} 

	if ( ACL_PRIV_ISSET(mask, ACL_PRIV_WRITE) ) {
		none = 0;
		*ptr++ = 'w';

	} else if ( ACL_PRIV_ISSET(mask, ACL_PRIV_WADD) ) {
		none = 0;
		*ptr++ = 'a';

	} else if ( ACL_PRIV_ISSET(mask, ACL_PRIV_WDEL) ) {
		none = 0;
		*ptr++ = 'z';
	} 

	if ( ACL_PRIV_ISSET(mask, ACL_PRIV_READ) ) {
		none = 0;
		*ptr++ = 'r';
	} 

	if ( ACL_PRIV_ISSET(mask, ACL_PRIV_SEARCH) ) {
		none = 0;
		*ptr++ = 's';
	} 

	if ( ACL_PRIV_ISSET(mask, ACL_PRIV_COMPARE) ) {
		none = 0;
		*ptr++ = 'c';
	} 

	if ( ACL_PRIV_ISSET(mask, ACL_PRIV_AUTH) ) {
		none = 0;
		*ptr++ = 'x';
	} 

	if ( ACL_PRIV_ISSET(mask, ACL_PRIV_DISCLOSE) ) {
		none = 0;
		*ptr++ = 'd';
	} 

	if ( none && ACL_PRIV_ISSET(mask, ACL_PRIV_NONE) ) {
		none = 0;
		*ptr++ = '0';
	} 

	if ( none ) {
		ptr = buf;
	}

	if ( ACL_IS_LEVEL( mask ) ) {
		*ptr++ = ')';
	}

	*ptr = '\0';

	return buf;
}

slap_mask_t
str2accessmask( const char *str )
{
	slap_mask_t	mask;

	if( !ASCII_ALPHA(str[0]) ) {
		int i;

		if ( str[0] == '=' ) {
			ACL_INIT(mask);

		} else if( str[0] == '+' ) {
			ACL_PRIV_ASSIGN(mask, ACL_PRIV_ADDITIVE);

		} else if( str[0] == '-' ) {
			ACL_PRIV_ASSIGN(mask, ACL_PRIV_SUBSTRACTIVE);

		} else {
			ACL_INVALIDATE(mask);
			return mask;
		}

		for( i=1; str[i] != '\0'; i++ ) {
			if( TOLOWER((unsigned char) str[i]) == 'm' ) {
				ACL_PRIV_SET(mask, ACL_PRIV_MANAGE);

			} else if( TOLOWER((unsigned char) str[i]) == 'w' ) {
				ACL_PRIV_SET(mask, ACL_PRIV_WRITE);

			} else if( TOLOWER((unsigned char) str[i]) == 'a' ) {
				ACL_PRIV_SET(mask, ACL_PRIV_WADD);

			} else if( TOLOWER((unsigned char) str[i]) == 'z' ) {
				ACL_PRIV_SET(mask, ACL_PRIV_WDEL);

			} else if( TOLOWER((unsigned char) str[i]) == 'r' ) {
				ACL_PRIV_SET(mask, ACL_PRIV_READ);

			} else if( TOLOWER((unsigned char) str[i]) == 's' ) {
				ACL_PRIV_SET(mask, ACL_PRIV_SEARCH);

			} else if( TOLOWER((unsigned char) str[i]) == 'c' ) {
				ACL_PRIV_SET(mask, ACL_PRIV_COMPARE);

			} else if( TOLOWER((unsigned char) str[i]) == 'x' ) {
				ACL_PRIV_SET(mask, ACL_PRIV_AUTH);

			} else if( TOLOWER((unsigned char) str[i]) == 'd' ) {
				ACL_PRIV_SET(mask, ACL_PRIV_DISCLOSE);

			} else if( str[i] != '0' ) {
				ACL_INVALIDATE(mask);
				return mask;
			}
		}

		return mask;
	}

	if ( strcasecmp( str, "none" ) == 0 ) {
		ACL_LVL_ASSIGN_NONE(mask);

	} else if ( strcasecmp( str, "disclose" ) == 0 ) {
		ACL_LVL_ASSIGN_DISCLOSE(mask);

	} else if ( strcasecmp( str, "auth" ) == 0 ) {
		ACL_LVL_ASSIGN_AUTH(mask);

	} else if ( strcasecmp( str, "compare" ) == 0 ) {
		ACL_LVL_ASSIGN_COMPARE(mask);

	} else if ( strcasecmp( str, "search" ) == 0 ) {
		ACL_LVL_ASSIGN_SEARCH(mask);

	} else if ( strcasecmp( str, "read" ) == 0 ) {
		ACL_LVL_ASSIGN_READ(mask);

	} else if ( strcasecmp( str, "add" ) == 0 ) {
		ACL_LVL_ASSIGN_WADD(mask);

	} else if ( strcasecmp( str, "delete" ) == 0 ) {
		ACL_LVL_ASSIGN_WDEL(mask);

	} else if ( strcasecmp( str, "write" ) == 0 ) {
		ACL_LVL_ASSIGN_WRITE(mask);

	} else if ( strcasecmp( str, "manage" ) == 0 ) {
		ACL_LVL_ASSIGN_MANAGE(mask);

	} else {
		ACL_INVALIDATE( mask );
	}

	return mask;
}

static void
acl_usage( void )
{
	fprintf( stderr, "%s%s%s\n",
		"<access clause> ::= access to <what> "
				"[ by <who> <access> [ <control> ] ]+ \n"
		"<what> ::= * | [dn[.<dnstyle>]=<DN>] [filter=<filter>] [attrs=<attrlist>]\n"
		"<attrlist> ::= <attr> [val[/matchingRule][.<attrstyle>]=<value>] | <attr> , <attrlist>\n"
		"<attr> ::= <attrname> | entry | children\n",
		"<who> ::= [ * | anonymous | users | self | dn[.<dnstyle>]=<DN> ]\n"
			"\t[ realanonymous | realusers | realself | realdn[.<dnstyle>]=<DN> ]\n"
			"\t[dnattr=<attrname>]\n"
			"\t[realdnattr=<attrname>]\n"
			"\t[group[/<objectclass>[/<attrname>]][.<style>]=<group>]\n"
			"\t[peername[.<peernamestyle>]=<peer>] [sockname[.<style>]=<name>]\n"
			"\t[domain[.<domainstyle>]=<domain>] [sockurl[.<style>]=<url>]\n"
#ifdef SLAPD_ACI_ENABLED
			"\t[aci=[<attrname>]]\n"
#endif
#ifdef SLAP_DYNACL
			"\t[dynacl/<name>[.<dynstyle>][=<pattern>]]\n"
#endif /* SLAP_DYNACL */
			"\t[ssf=<n>] [transport_ssf=<n>] [tls_ssf=<n>] [sasl_ssf=<n>]\n",
		"<style> ::= exact | regex | base(Object)\n"
		"<dnstyle> ::= base(Object) | one(level) | sub(tree) | children | "
			"exact | regex\n"
		"<attrstyle> ::= exact | regex | base(Object) | one(level) | "
			"sub(tree) | children\n"
		"<peernamestyle> ::= exact | regex | ip | path\n"
		"<domainstyle> ::= exact | regex | base(Object) | sub(tree)\n"
		"<access> ::= [[real]self]{<level>|<priv>}\n"
		"<level> ::= none|disclose|auth|compare|search|read|{write|add|delete}|manage\n"
		"<priv> ::= {=|+|-}{0|d|x|c|s|r|{w|a|z}|m}+\n"
		"<control> ::= [ stop | continue | break ]\n"
	);
	exit( EXIT_FAILURE );
}

/*
 * Set pattern to a "normalized" DN from src.
 * At present it simply eats the (optional) space after 
 * a RDN separator (,)
 * Eventually will evolve in a more complete normalization
 */
static void
acl_regex_normalized_dn(
	const char *src,
	struct berval *pattern )
{
	char *str, *p;
	ber_len_t len;

	str = ch_strdup( src );
	len = strlen( src );

	for ( p = str; p && p[0]; p++ ) {
		/* escape */
		if ( p[0] == '\\' && p[1] ) {
			/* 
			 * if escaping a hex pair we should
			 * increment p twice; however, in that 
			 * case the second hex number does 
			 * no harm
			 */
			p++;
		}

		if ( p[0] == ',' && p[1] == ' ' ) {
			char *q;
			
			/*
			 * too much space should be an error if we are pedantic
			 */
			for ( q = &p[2]; q[0] == ' '; q++ ) {
				/* DO NOTHING */ ;
			}
			AC_MEMCPY( p+1, q, len-(q-str)+1);
		}
	}
	pattern->bv_val = str;
	pattern->bv_len = p - str;

	return;
}

static void
split(
    char	*line,
    int		splitchar,
    char	**left,
    char	**right )
{
	*left = line;
	if ( (*right = strchr( line, splitchar )) != NULL ) {
		*((*right)++) = '\0';
	}
}

static void
access_append( Access **l, Access *a )
{
	for ( ; *l != NULL; l = &(*l)->a_next ) {
		;	/* Empty */
	}

	*l = a;
}

void
acl_append( AccessControl **l, AccessControl *a, int pos )
{
	int i;

	for (i=0 ; i != pos && *l != NULL; l = &(*l)->acl_next, i++ ) {
		;	/* Empty */
	}
	if ( *l && a )
		a->acl_next = *l;
	*l = a;
}

static void
access_free( Access *a )
{
	if ( !BER_BVISNULL( &a->a_dn_pat ) ) {
		free( a->a_dn_pat.bv_val );
	}
	if ( !BER_BVISNULL( &a->a_realdn_pat ) ) {
		free( a->a_realdn_pat.bv_val );
	}
	if ( !BER_BVISNULL( &a->a_peername_pat ) ) {
		free( a->a_peername_pat.bv_val );
	}
	if ( !BER_BVISNULL( &a->a_sockname_pat ) ) {
		free( a->a_sockname_pat.bv_val );
	}
	if ( !BER_BVISNULL( &a->a_domain_pat ) ) {
		free( a->a_domain_pat.bv_val );
	}
	if ( !BER_BVISNULL( &a->a_sockurl_pat ) ) {
		free( a->a_sockurl_pat.bv_val );
	}
	if ( !BER_BVISNULL( &a->a_set_pat ) ) {
		free( a->a_set_pat.bv_val );
	}
	if ( !BER_BVISNULL( &a->a_group_pat ) ) {
		free( a->a_group_pat.bv_val );
	}
	if ( a->a_dynacl != NULL ) {
		slap_dynacl_t	*da;
		for ( da = a->a_dynacl; da; ) {
			slap_dynacl_t	*tmp = da;

			da = da->da_next;

			if ( tmp->da_destroy ) {
				tmp->da_destroy( tmp->da_private );
			}

			ch_free( tmp );
		}
	}
	free( a );
}

void
acl_free( AccessControl *a )
{
	Access *n;
	AttributeName *an;

	if ( a->acl_filter ) {
		filter_free( a->acl_filter );
	}
	if ( !BER_BVISNULL( &a->acl_dn_pat ) ) {
		if ( a->acl_dn_style == ACL_STYLE_REGEX ) {
			regfree( &a->acl_dn_re );
		}
		free ( a->acl_dn_pat.bv_val );
	}
	if ( a->acl_attrs ) {
		for ( an = a->acl_attrs; !BER_BVISNULL( &an->an_name ); an++ ) {
			free( an->an_name.bv_val );
		}
		free( a->acl_attrs );
	}
	for ( ; a->acl_access; a->acl_access = n ) {
		n = a->acl_access->a_next;
		access_free( a->acl_access );
	}
	free( a );
}

/* Because backend_startup uses acl_append to tack on the global_acl to
 * the end of each backend's acl, we cannot just take one argument and
 * merrily free our way to the end of the list. backend_destroy calls us
 * with the be_acl in arg1, and global_acl in arg2 to give us a stopping
 * point. config_destroy calls us with global_acl in arg1 and NULL in
 * arg2, so we then proceed to polish off the global_acl.
 */
void
acl_destroy( AccessControl *a, AccessControl *end )
{
	AccessControl *n;

	for ( ; a && a != end; a = n ) {
		n = a->acl_next;
		acl_free( a );
	}
}

char *
access2str( slap_access_t access )
{
	if ( access == ACL_NONE ) {
		return "none";

	} else if ( access == ACL_DISCLOSE ) {
		return "disclose";

	} else if ( access == ACL_AUTH ) {
		return "auth";

	} else if ( access == ACL_COMPARE ) {
		return "compare";

	} else if ( access == ACL_SEARCH ) {
		return "search";

	} else if ( access == ACL_READ ) {
		return "read";

	} else if ( access == ACL_WRITE ) {
		return "write";

	} else if ( access == ACL_WADD ) {
		return "add";

	} else if ( access == ACL_WDEL ) {
		return "delete";

	} else if ( access == ACL_MANAGE ) {
		return "manage";

	}

	return "unknown";
}

slap_access_t
str2access( const char *str )
{
	if ( strcasecmp( str, "none" ) == 0 ) {
		return ACL_NONE;

	} else if ( strcasecmp( str, "disclose" ) == 0 ) {
#ifndef SLAP_ACL_HONOR_DISCLOSE
		fprintf( stderr, "str2access: warning, "
			"\"disclose\" privilege disabled.\n" );
#endif /* SLAP_ACL_HONOR_DISCLOSE */
		return ACL_DISCLOSE;

	} else if ( strcasecmp( str, "auth" ) == 0 ) {
		return ACL_AUTH;

	} else if ( strcasecmp( str, "compare" ) == 0 ) {
		return ACL_COMPARE;

	} else if ( strcasecmp( str, "search" ) == 0 ) {
		return ACL_SEARCH;

	} else if ( strcasecmp( str, "read" ) == 0 ) {
		return ACL_READ;

	} else if ( strcasecmp( str, "write" ) == 0 ) {
		return ACL_WRITE;

	} else if ( strcasecmp( str, "add" ) == 0 ) {
		return ACL_WADD;

	} else if ( strcasecmp( str, "delete" ) == 0 ) {
		return ACL_WDEL;

	} else if ( strcasecmp( str, "manage" ) == 0 ) {
		return ACL_MANAGE;
	}

	return( ACL_INVALID_ACCESS );
}

#define ACLBUF_MAXLEN	8192

static char aclbuf[ACLBUF_MAXLEN];

static char *
dnaccess2text( slap_dn_access *bdn, char *ptr, int is_realdn )
{
	*ptr++ = ' ';

	if ( is_realdn ) {
		ptr = lutil_strcopy( ptr, "real" );
	}

	if ( ber_bvccmp( &bdn->a_pat, '*' ) ||
		bdn->a_style == ACL_STYLE_ANONYMOUS ||
		bdn->a_style == ACL_STYLE_USERS ||
		bdn->a_style == ACL_STYLE_SELF )
	{
		if ( is_realdn ) {
			assert( ! ber_bvccmp( &bdn->a_pat, '*' ) );
		}
			
		ptr = lutil_strcopy( ptr, bdn->a_pat.bv_val );
		if ( bdn->a_style == ACL_STYLE_SELF && bdn->a_self_level != 0 ) {
			int n = sprintf( ptr, ".level{%d}", bdn->a_self_level );
			if ( n > 0 ) {
				ptr += n;
			} /* else ? */
		}

	} else {
		ptr = lutil_strcopy( ptr, "dn." );
		if ( bdn->a_style == ACL_STYLE_BASE )
			ptr = lutil_strcopy( ptr, style_base );
		else 
			ptr = lutil_strcopy( ptr, style_strings[bdn->a_style] );
		if ( bdn->a_style == ACL_STYLE_LEVEL ) {
			int n = sprintf( ptr, "{%d}", bdn->a_level );
			if ( n > 0 ) {
				ptr += n;
			} /* else ? */
		}
		if ( bdn->a_expand ) {
			ptr = lutil_strcopy( ptr, ",expand" );
		}
		*ptr++ = '=';
		*ptr++ = '"';
		ptr = lutil_strcopy( ptr, bdn->a_pat.bv_val );
		*ptr++ = '"';
	}
	return ptr;
}

static char *
access2text( Access *b, char *ptr )
{
	char maskbuf[ACCESSMASK_MAXLEN];

	ptr = lutil_strcopy( ptr, "\tby" );

	if ( !BER_BVISEMPTY( &b->a_dn_pat ) ) {
		ptr = dnaccess2text( &b->a_dn, ptr, 0 );
	}
	if ( b->a_dn_at ) {
		ptr = lutil_strcopy( ptr, " dnattr=" );
		ptr = lutil_strcopy( ptr, b->a_dn_at->ad_cname.bv_val );
	}

	if ( !BER_BVISEMPTY( &b->a_realdn_pat ) ) {
		ptr = dnaccess2text( &b->a_realdn, ptr, 1 );
	}
	if ( b->a_realdn_at ) {
		ptr = lutil_strcopy( ptr, " realdnattr=" );
		ptr = lutil_strcopy( ptr, b->a_realdn_at->ad_cname.bv_val );
	}

	if ( !BER_BVISEMPTY( &b->a_group_pat ) ) {
		ptr = lutil_strcopy( ptr, " group/" );
		ptr = lutil_strcopy( ptr, b->a_group_oc ?
			b->a_group_oc->soc_cname.bv_val : SLAPD_GROUP_CLASS );
		*ptr++ = '/';
		ptr = lutil_strcopy( ptr, b->a_group_at ?
			b->a_group_at->ad_cname.bv_val : SLAPD_GROUP_ATTR );
		*ptr++ = '.';
		ptr = lutil_strcopy( ptr, style_strings[b->a_group_style] );
		*ptr++ = '=';
		*ptr++ = '"';
		ptr = lutil_strcopy( ptr, b->a_group_pat.bv_val );
		*ptr++ = '"';
	}

	if ( !BER_BVISEMPTY( &b->a_peername_pat ) ) {
		ptr = lutil_strcopy( ptr, " peername" );
		*ptr++ = '.';
		ptr = lutil_strcopy( ptr, style_strings[b->a_peername_style] );
		*ptr++ = '=';
		*ptr++ = '"';
		ptr = lutil_strcopy( ptr, b->a_peername_pat.bv_val );
		*ptr++ = '"';
	}

	if ( !BER_BVISEMPTY( &b->a_sockname_pat ) ) {
		ptr = lutil_strcopy( ptr, " sockname" );
		*ptr++ = '.';
		ptr = lutil_strcopy( ptr, style_strings[b->a_sockname_style] );
		*ptr++ = '=';
		*ptr++ = '"';
		ptr = lutil_strcopy( ptr, b->a_sockname_pat.bv_val );
		*ptr++ = '"';
	}

	if ( !BER_BVISEMPTY( &b->a_domain_pat ) ) {
		ptr = lutil_strcopy( ptr, " domain" );
		*ptr++ = '.';
		ptr = lutil_strcopy( ptr, style_strings[b->a_domain_style] );
		if ( b->a_domain_expand ) {
			ptr = lutil_strcopy( ptr, ",expand" );
		}
		*ptr++ = '=';
		ptr = lutil_strcopy( ptr, b->a_domain_pat.bv_val );
	}

	if ( !BER_BVISEMPTY( &b->a_sockurl_pat ) ) {
		ptr = lutil_strcopy( ptr, " sockurl" );
		*ptr++ = '.';
		ptr = lutil_strcopy( ptr, style_strings[b->a_sockurl_style] );
		*ptr++ = '=';
		*ptr++ = '"';
		ptr = lutil_strcopy( ptr, b->a_sockurl_pat.bv_val );
		*ptr++ = '"';
	}

	if ( !BER_BVISEMPTY( &b->a_set_pat ) ) {
		ptr = lutil_strcopy( ptr, " set" );
		*ptr++ = '.';
		ptr = lutil_strcopy( ptr, style_strings[b->a_set_style] );
		*ptr++ = '=';
		*ptr++ = '"';
		ptr = lutil_strcopy( ptr, b->a_set_pat.bv_val );
		*ptr++ = '"';
	}

#ifdef SLAP_DYNACL
	if ( b->a_dynacl ) {
		slap_dynacl_t	*da;

		for ( da = b->a_dynacl; da; da = da->da_next ) {
			if ( da->da_unparse ) {
				struct berval bv = BER_BVNULL;
				(void)( *da->da_unparse )( da->da_private, &bv );
				assert( !BER_BVISNULL( &bv ) );
				ptr = lutil_strcopy( ptr, bv.bv_val );
				ch_free( bv.bv_val );
			}
		}
	}
#else /* ! SLAP_DYNACL */
#ifdef SLAPD_ACI_ENABLED
	if ( b->a_aci_at != NULL ) {
		ptr = lutil_strcopy( ptr, " aci=" );
		ptr = lutil_strcopy( ptr, b->a_aci_at->ad_cname.bv_val );
	}
#endif
#endif /* SLAP_DYNACL */

	/* Security Strength Factors */
	if ( b->a_authz.sai_ssf ) {
		ptr += sprintf( ptr, " ssf=%u", 
			b->a_authz.sai_ssf );
	}
	if ( b->a_authz.sai_transport_ssf ) {
		ptr += sprintf( ptr, " transport_ssf=%u",
			b->a_authz.sai_transport_ssf );
	}
	if ( b->a_authz.sai_tls_ssf ) {
		ptr += sprintf( ptr, " tls_ssf=%u",
			b->a_authz.sai_tls_ssf );
	}
	if ( b->a_authz.sai_sasl_ssf ) {
		ptr += sprintf( ptr, " sasl_ssf=%u",
			b->a_authz.sai_sasl_ssf );
	}

	*ptr++ = ' ';
	if ( b->a_dn_self ) {
		ptr = lutil_strcopy( ptr, "self" );
	} else if ( b->a_realdn_self ) {
		ptr = lutil_strcopy( ptr, "realself" );
	}
	ptr = lutil_strcopy( ptr, accessmask2str( b->a_access_mask, maskbuf, 0 ));
	if ( !maskbuf[0] ) ptr--;

	if( b->a_type == ACL_BREAK ) {
		ptr = lutil_strcopy( ptr, " break" );

	} else if( b->a_type == ACL_CONTINUE ) {
		ptr = lutil_strcopy( ptr, " continue" );

	} else if( b->a_type != ACL_STOP ) {
		ptr = lutil_strcopy( ptr, " unknown-control" );
	} else {
		if ( !maskbuf[0] ) ptr = lutil_strcopy( ptr, " stop" );
	}
	*ptr++ = '\n';

	return ptr;
}

void
acl_unparse( AccessControl *a, struct berval *bv )
{
	Access	*b;
	char	*ptr;
	int	to = 0;

	bv->bv_val = aclbuf;
	bv->bv_len = 0;

	ptr = bv->bv_val;

	ptr = lutil_strcopy( ptr, "to" );
	if ( !BER_BVISNULL( &a->acl_dn_pat ) ) {
		to++;
		ptr = lutil_strcopy( ptr, " dn." );
		if ( a->acl_dn_style == ACL_STYLE_BASE )
			ptr = lutil_strcopy( ptr, style_base );
		else
			ptr = lutil_strcopy( ptr, style_strings[a->acl_dn_style] );
		*ptr++ = '=';
		*ptr++ = '"';
		ptr = lutil_strcopy( ptr, a->acl_dn_pat.bv_val );
		ptr = lutil_strcopy( ptr, "\"\n" );
	}

	if ( a->acl_filter != NULL ) {
		struct berval	bv = BER_BVNULL;

		to++;
		filter2bv( a->acl_filter, &bv );
		ptr = lutil_strcopy( ptr, " filter=\"" );
		ptr = lutil_strcopy( ptr, bv.bv_val );
		*ptr++ = '"';
		*ptr++ = '\n';
		ch_free( bv.bv_val );
	}

	if ( a->acl_attrs != NULL ) {
		int	first = 1;
		AttributeName *an;
		to++;

		ptr = lutil_strcopy( ptr, " attrs=" );
		for ( an = a->acl_attrs; an && !BER_BVISNULL( &an->an_name ); an++ ) {
			if ( ! first ) *ptr++ = ',';
			if (an->an_oc) {
				*ptr++ = an->an_oc_exclude ? '!' : '@';
				ptr = lutil_strcopy( ptr, an->an_oc->soc_cname.bv_val );

			} else {
				ptr = lutil_strcopy( ptr, an->an_name.bv_val );
			}
			first = 0;
		}
		*ptr++ = '\n';
	}

	if ( !BER_BVISEMPTY( &a->acl_attrval ) ) {
		to++;
		ptr = lutil_strcopy( ptr, " val." );
		if ( a->acl_attrval_style == ACL_STYLE_BASE &&
			a->acl_attrs[0].an_desc->ad_type->sat_syntax ==
				slap_schema.si_syn_distinguishedName )
			ptr = lutil_strcopy( ptr, style_base );
		else
			ptr = lutil_strcopy( ptr, style_strings[a->acl_attrval_style] );
		*ptr++ = '=';
		*ptr++ = '"';
		ptr = lutil_strcopy( ptr, a->acl_attrval.bv_val );
		*ptr++ = '"';
		*ptr++ = '\n';
	}

	if( !to ) {
		ptr = lutil_strcopy( ptr, " *\n" );
	}

	for ( b = a->acl_access; b != NULL; b = b->a_next ) {
		ptr = access2text( b, ptr );
	}
	*ptr = '\0';
	bv->bv_len = ptr - bv->bv_val;
}

#ifdef LDAP_DEBUG

static void
print_acl( Backend *be, AccessControl *a )
{
	struct berval bv;

	acl_unparse( a, &bv );
	fprintf( stderr, "%s ACL: access %s\n",
		be == NULL ? "Global" : "Backend", bv.bv_val );
}
#endif /* LDAP_DEBUG */
