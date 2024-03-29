/**
 * Copyright (C) 2012-2014 Steven Barth <steven@midlink.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License v2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <stdio.h>
#include <netdb.h>
#include <resolv.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <unistd.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "odhcp6c.h"

static const char hexdigits[] = "0123456789abcdef";
static const int8_t hexvals[] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -2, -2, -1, -1, -2, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, -1, -1, -1, -1, -1, -1,
    -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};



static char *argv[4] = {NULL, NULL, NULL, NULL};
static volatile char *delayed_call = NULL;
static bool dont_delay = false;


int script_init(const char *path, const char *ifname)
{
	argv[0] = (char*)path;
	argv[1] = (char*)ifname;
	return 0;
}


ssize_t script_unhexlify(uint8_t *dst, size_t len, const char *src)
{
	size_t c;
	for (c = 0; c < len && src[0] && src[1]; ++c) {
		int8_t x = (int8_t)*src++;
		int8_t y = (int8_t)*src++;
		if (x < 0 || (x = hexvals[x]) < 0
				|| y < 0 || (y = hexvals[y]) < 0)
			return -1;
		dst[c] = x << 4 | y;
		while (((int8_t)*src) < 0 ||
				(*src && hexvals[(uint8_t)*src] < 0))
			src++;
	}

	return c;
}


static void script_hexlify(char *dst, const uint8_t *src, size_t len) {
	for (size_t i = 0; i < len; ++i) {
		*dst++ = hexdigits[src[i] >> 4];
		*dst++ = hexdigits[src[i] & 0x0f];
	}
	*dst = 0;
}


static void ipv6_to_env(const char *name,
		const struct in6_addr *addr, size_t cnt)
{
	size_t buf_len = strlen(name);
	char *buf = realloc(NULL, cnt * INET6_ADDRSTRLEN + buf_len + 2);
	memcpy(buf, name, buf_len);
	buf[buf_len++] = '=';
	for (size_t i = 0; i < cnt; ++i) {
		inet_ntop(AF_INET6, &addr[i], &buf[buf_len], INET6_ADDRSTRLEN);
		buf_len += strlen(&buf[buf_len]);
		buf[buf_len++] = ' ';
	}
	buf[buf_len - 1] = '\0';
	putenv(buf);
}


static void fqdn_to_env(const char *name, const uint8_t *fqdn, size_t len)
{
	size_t buf_len = strlen(name);
	size_t buf_size = len + buf_len + 2;
	const uint8_t *fqdn_end = fqdn + len;
	char *buf = realloc(NULL, len + buf_len + 2);
	memcpy(buf, name, buf_len);
	buf[buf_len++] = '=';
	int l = 1;
	while (l > 0 && fqdn < fqdn_end) {
		l = dn_expand(fqdn, fqdn_end, fqdn, &buf[buf_len], buf_size - buf_len);
		fqdn += l;
		buf_len += strlen(&buf[buf_len]);
		buf[buf_len++] = ' ';
	}
	buf[buf_len - 1] = '\0';
	putenv(buf);
}


static void fqdn_to_ip_env(const char *name, const uint8_t *fqdn, size_t len)
{
	size_t buf_len = strlen(name);
	char *buf = realloc(NULL, INET6_ADDRSTRLEN + buf_len + 3);
	memcpy(buf, name, buf_len);
	buf[buf_len++] = '=';

	char namebuf[256];
	if (dn_expand(fqdn, fqdn + len, fqdn, namebuf, sizeof(namebuf)) <= 0)
		return;

	struct addrinfo hints = {.ai_family = AF_INET6}, *r;
	if (getaddrinfo(namebuf, NULL, &hints, &r))
		return;

	struct sockaddr_in6 *sin6 = (struct sockaddr_in6*)r->ai_addr;
	inet_ntop(AF_INET6, &sin6->sin6_addr, &buf[buf_len], INET6_ADDRSTRLEN);

	freeaddrinfo(r);
	putenv(buf);
}


static void bin_to_env(uint8_t *opts, size_t len)
{
	uint8_t *oend = opts + len, *odata;
	uint16_t otype, olen;
	dhcpv6_for_each_option(opts, oend, otype, olen, odata) {
		char *buf = realloc(NULL, 14 + (olen * 2));
		size_t buf_len = 0;

		snprintf(buf, 14, "OPTION_%hu=", otype);
		buf_len += strlen(buf);

		script_hexlify(&buf[buf_len], odata, olen);
		putenv(buf);
	}
}

enum entry_type {
	ENTRY_ADDRESS,
	ENTRY_HOST,
	ENTRY_ROUTE,
	ENTRY_PREFIX
};

static void entry_to_env(const char *name, const void *data, size_t len, enum entry_type type)
{
	size_t buf_len = strlen(name);
	const struct odhcp6c_entry *e = data;
	char *buf = realloc(NULL, buf_len + 2 + (len / sizeof(*e)) * 144);
	memcpy(buf, name, buf_len);
	buf[buf_len++] = '=';

	for (size_t i = 0; i < len / sizeof(*e); ++i) {
		inet_ntop(AF_INET6, &e[i].target, &buf[buf_len], INET6_ADDRSTRLEN);
		buf_len += strlen(&buf[buf_len]);
		if (type != ENTRY_HOST) {
			buf_len += snprintf(&buf[buf_len], 6, "/%"PRIu16, e[i].length);
			if (type == ENTRY_ROUTE) {
				buf[buf_len++] = ',';
				if (!IN6_IS_ADDR_UNSPECIFIED(&e[i].router)) {
					inet_ntop(AF_INET6, &e[i].router, &buf[buf_len], INET6_ADDRSTRLEN);
					buf_len += strlen(&buf[buf_len]);
				}
				buf_len += snprintf(&buf[buf_len], 24, ",%u", e[i].valid);
				buf_len += snprintf(&buf[buf_len], 12, ",%u", e[i].priority);
			} else {
				buf_len += snprintf(&buf[buf_len], 24, ",%u,%u", e[i].preferred, e[i].valid);
			}

			if ((type == ENTRY_PREFIX || type == ENTRY_ADDRESS) && e[i].class)
				buf_len += snprintf(&buf[buf_len], 12, ",class=%u", e[i].class);
			else if (type == ENTRY_PREFIX && ntohl(e[i].iaid) != 1)
				buf_len += snprintf(&buf[buf_len], 16, ",class=%08x", ntohl(e[i].iaid));

			if (type == ENTRY_PREFIX && e[i].priority) {
				// priority and router are abused for prefix exclusion
				buf_len += snprintf(&buf[buf_len], 12, ",excluded=");
				inet_ntop(AF_INET6, &e[i].router, &buf[buf_len], INET6_ADDRSTRLEN);
				buf_len += strlen(&buf[buf_len]);
				buf_len += snprintf(&buf[buf_len], 24, "/%u", e[i].priority);
			}
		}
		buf[buf_len++] = ' ';
	}

	buf[buf_len - 1] = '\0';
	putenv(buf);
}

#ifdef EXT_S46
static void s46_to_env_portparams(const uint8_t *data, size_t len, FILE *fp)
{
	uint8_t *odata;
	uint16_t otype, olen;
	dhcpv6_for_each_option(data, &data[len], otype, olen, odata) {
		if (otype == DHCPV6_OPT_S46_PORTPARAMS &&
				olen == sizeof(struct dhcpv6_s46_portparams)) {
			struct dhcpv6_s46_portparams *params = (void*)odata;
			fprintf(fp, "offset=%d,psidlen=%d,psid=%d,",
					params->offset, params->psid_len, ntohs(params->psid));
		}
	}
}
#endif

static void s46_to_env(enum odhcp6c_state state, const uint8_t *data, size_t len)
{
	const char *name = (state == STATE_S46_MAPE) ? "MAPE" :
			(state == STATE_S46_MAPT) ? "MAPT" : "LW4O6";

	char *str;
	size_t strsize;

	FILE *fp = open_memstream(&str, &strsize);
	fputs(name, fp);
	fputc('=', fp);

#ifdef EXT_S46
	uint8_t *odata;
	uint16_t otype, olen;
	dhcpv6_for_each_option(data, &data[len], otype, olen, odata) {
		struct dhcpv6_s46_rule *rule = (struct dhcpv6_s46_rule*)odata;
		struct dhcpv6_s46_dmr *dmr = (struct dhcpv6_s46_dmr*)odata;
		struct dhcpv6_s46_v4v6bind *bind = (struct dhcpv6_s46_v4v6bind*)odata;

		if (state != STATE_S46_LW && otype == DHCPV6_OPT_S46_RULE &&
				olen >= sizeof(struct dhcpv6_s46_rule) && olen >=
				sizeof(struct dhcpv6_s46_rule) + rule->prefix6_len) {
			char buf4[INET_ADDRSTRLEN];
			char buf6[INET6_ADDRSTRLEN];
			struct in6_addr in6 = IN6ADDR_ANY_INIT;
			memcpy(&in6, rule->ipv6_prefix, rule->prefix6_len);

			inet_ntop(AF_INET, &rule->ipv4_prefix, buf4, sizeof(buf4));
			inet_ntop(AF_INET6, &in6, buf6, sizeof(buf6));

			if (rule->flags & 1)
				fputs("fmr,", fp);

			fprintf(fp, "ealen=%d,prefix4len=%d,prefix6len=%d,ipv4prefix=%s,ipv6prefix=%s,",
					rule->ea_len, rule->prefix4_len, rule->prefix6_len, buf4, buf6);

			s46_to_env_portparams(&rule->ipv6_prefix[rule->prefix6_len],
					olen - sizeof(*rule) - rule->prefix6_len, fp);

			dhcpv6_for_each_option(data, &data[len], otype, olen, odata) {
				if (state != STATE_S46_MAPT && otype == DHCPV6_OPT_S46_BR &&
						olen == sizeof(struct in6_addr)) {
					inet_ntop(AF_INET6, odata, buf6, sizeof(buf6));
					fprintf(fp, "br=%s,", buf6);
				} else if (state == STATE_S46_MAPT && otype == DHCPV6_OPT_S46_DMR &&
						olen >= sizeof(struct dhcpv6_s46_dmr) && olen >=
						sizeof(struct dhcpv6_s46_dmr) + dmr->dmr_prefix6_len) {
					memset(&in6, 0, sizeof(in6));
					memcpy(&in6, dmr->dmr_ipv6_prefix, dmr->dmr_prefix6_len);
					inet_ntop(AF_INET6, &in6, buf6, sizeof(buf6));
					fprintf(fp, "dmr=%s/%d,", buf6, dmr->dmr_prefix6_len);
				}
			}
		} else if (state == STATE_S46_LW && otype == DHCPV6_OPT_S46_V4V6BIND &&
				olen >= sizeof(struct dhcpv6_s46_v4v6bind) && olen >=
				sizeof(struct dhcpv6_s46_v4v6bind) + bind->bindprefix6_len) {
			char buf4[INET_ADDRSTRLEN];
			char buf6[INET6_ADDRSTRLEN];
			struct in6_addr in6 = IN6ADDR_ANY_INIT;
			memcpy(&in6, bind->bind_ipv6_prefix, bind->bindprefix6_len);

			inet_ntop(AF_INET, &bind->ipv4_address, buf4, sizeof(buf4));
			inet_ntop(AF_INET6, &in6, buf6, sizeof(buf6));

			fprintf(fp, "ipv4address=%s,prefix6len=%d,ipv6prefix=%s,",
					buf4, bind->bindprefix6_len, buf6);

			s46_to_env_portparams(&bind->bind_ipv6_prefix[bind->bindprefix6_len],
					olen - sizeof(*bind) - bind->bindprefix6_len, fp);

			dhcpv6_for_each_option(data, &data[len], otype, olen, odata) {
				if (otype == DHCPV6_OPT_S46_BR && olen == sizeof(struct in6_addr)) {
					inet_ntop(AF_INET6, odata, buf6, sizeof(buf6));
					fprintf(fp, "br=%s,", buf6);
				}
			}
		}

		fputc(' ', fp);
	}
#else
	if (data && len) {}
#endif

	fclose(fp);
	putenv(str);
}


static void script_call_delayed(int signal __attribute__((unused)))
{
	if (delayed_call)
		script_call((char*)delayed_call);
}


void script_delay_call(const char *status, int timeout)
{
	if (dont_delay) {
		script_call(status);
	} else if (!delayed_call) {
		delayed_call = strdup(status);
		signal(SIGALRM, script_call_delayed);
		alarm(timeout);
	}
}


void script_call(const char *status)
{
	size_t dns_len, search_len, custom_len, sntp_ip_len, ntp_ip_len, ntp_dns_len, fos_88_len, fos_99_len, client_id_len;
	size_t sip_ip_len, sip_fqdn_len, aftr_name_len, cer_len, ncs_fqdn_len;
	size_t s46_mapt_len, s46_mape_len, s46_lw_len;

	odhcp6c_expire();
	if (delayed_call) {
		alarm(0);
		dont_delay = true;
	}

	struct in6_addr *dns = odhcp6c_get_state(STATE_DNS, &dns_len);
	struct in6_addr *fos_88 = odhcp6c_get_state(STATE_DHCP4O6_SERVERS_88, &fos_88_len);
	struct in6_addr *fos_99 = odhcp6c_get_state(STATE_DHCP4O6_SERVERS_99, &fos_99_len);
	uint8_t *search = odhcp6c_get_state(STATE_SEARCH, &search_len);
	uint8_t *custom = odhcp6c_get_state(STATE_CUSTOM_OPTS, &custom_len);
	struct in6_addr *sntp = odhcp6c_get_state(STATE_SNTP_IP, &sntp_ip_len);
	struct in6_addr *ntp = odhcp6c_get_state(STATE_NTP_IP, &ntp_ip_len);
	uint8_t *ntp_dns = odhcp6c_get_state(STATE_NTP_FQDN, &ntp_dns_len);
	struct in6_addr *sip = odhcp6c_get_state(STATE_SIP_IP, &sip_ip_len);
	uint8_t *sip_fqdn = odhcp6c_get_state(STATE_SIP_FQDN, &sip_fqdn_len);
	uint8_t *aftr_name = odhcp6c_get_state(STATE_AFTR_NAME, &aftr_name_len);
	struct in6_addr *cer = odhcp6c_get_state(STATE_CER, &cer_len);
	uint8_t *s46_mapt = odhcp6c_get_state(STATE_S46_MAPT, &s46_mapt_len);
	uint8_t *s46_mape = odhcp6c_get_state(STATE_S46_MAPE, &s46_mape_len);
	uint8_t *s46_lw = odhcp6c_get_state(STATE_S46_LW, &s46_lw_len);
	uint8_t *ncs_fqdn = odhcp6c_get_state(STATE_NCS_FQDN, &ncs_fqdn_len);

	size_t prefix_len, address_len, ra_pref_len, ra_route_len, ra_dns_len;
	uint8_t *prefix = odhcp6c_get_state(STATE_IA_PD, &prefix_len);
	uint8_t *address = odhcp6c_get_state(STATE_IA_NA, &address_len);
	uint8_t *ra_pref = odhcp6c_get_state(STATE_RA_PREFIX, &ra_pref_len);
	uint8_t *ra_route = odhcp6c_get_state(STATE_RA_ROUTE, &ra_route_len);
	uint8_t *ra_dns = odhcp6c_get_state(STATE_RA_DNS, &ra_dns_len);

	uint8_t *client_id = odhcp6c_get_state(STATE_CLIENT_ID, &client_id_len);

	// Don't set environment before forking, because env is leaky.
	if (fork() == 0) {
		ipv6_to_env("RDNSS", dns, dns_len / sizeof(*dns));
		ipv6_to_env("SNTP_IP", sntp, sntp_ip_len / sizeof(*sntp));
		ipv6_to_env("NTP_IP", ntp, ntp_ip_len / sizeof(*ntp));
		ipv6_to_env("DHCP4_O_DHCP6_SERVERS_88", fos_88, fos_88_len / sizeof(*fos_88));
		ipv6_to_env("DHCP4_O_DHCP6_SERVERS_99", fos_99, fos_99_len / sizeof(*fos_99));
		fqdn_to_env("NTP_FQDN", ntp_dns, ntp_dns_len);
		ipv6_to_env("SIP_IP", sip, sip_ip_len / sizeof(*sip));
		fqdn_to_env("DOMAINS", search, search_len);
		fqdn_to_env("SIP_DOMAIN", sip_fqdn, sip_fqdn_len);
		fqdn_to_env("AFTR", aftr_name, aftr_name_len);
		fqdn_to_env("NCS", ncs_fqdn, ncs_fqdn_len);
		fqdn_to_ip_env("AFTR_IP", aftr_name, aftr_name_len);
		ipv6_to_env("CER", cer, cer_len / sizeof(*cer));
		s46_to_env(STATE_S46_MAPE, s46_mape, s46_mape_len);
		s46_to_env(STATE_S46_MAPT, s46_mapt, s46_mapt_len);
		s46_to_env(STATE_S46_LW, s46_lw, s46_lw_len);
		bin_to_env(custom, custom_len);
		entry_to_env("PREFIXES", prefix, prefix_len, ENTRY_PREFIX);
		entry_to_env("ADDRESSES", address, address_len, ENTRY_ADDRESS);
		entry_to_env("RA_ADDRESSES", ra_pref, ra_pref_len, ENTRY_ADDRESS);
		entry_to_env("RA_ROUTES", ra_route, ra_route_len, ENTRY_ROUTE);
		entry_to_env("RA_DNS", ra_dns, ra_dns_len, ENTRY_HOST);

		bin_to_env(client_id, client_id_len); /* will be exported as "OPTION_1" into environment */

		argv[2] = (char*)status;
		execv(argv[0], argv);
		_exit(128);
	}

	// Delete lost prefixes and user opts
	odhcp6c_clear_state(STATE_CUSTOM_OPTS);
}
