/*
 * ZMap Copyright 2013 Regents of the University of Michigan
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 */

// probe module for performing TCP SYN scans with the MP_CAPABLE MP-TCP option
// Authors: Olivier Mehani <olivier.mehani@nicta.com.au>

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#include "../../lib/includes.h"
#include "../fieldset.h"
#include "probe_modules.h"
#include "packet.h"

#ifndef TCPOPT_MPTCP
# define TCPOPT_MPTCP 	30

#include <asm/byteorder.h>

/* From <net/mptcp.h> */


#define MPTCP_SUB_CAPABLE			0
#define MPTCP_SUB_LEN_CAPABLE_SYN		12

struct mptcp_option {
	__u8	kind;
	__u8	len;
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u8	ver:4,
		sub:4;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u8	sub:4,
		ver:4;
#else
#error	"Adjust your <asm/byteorder.h> defines"
#endif
};

struct mp_capable {
	__u8	kind;
	__u8	len;
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u8	ver:4,
		sub:4;
	__u8	h:1,
		rsv:5,
		b:1,
		a:1;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u8	sub:4,
		ver:4;
	__u8	a:1,
		b:1,
		rsv:5,
		h:1;
#else
#error	"Adjust your <asm/byteorder.h> defines"
#endif
	__u64	sender_key;
  __u64	receiver_key;
} __attribute__((__packed__));
#endif /* TCPOPT_MPTCP */

probe_module_t module_tcp_synscan;
static uint32_t num_ports;

int synscan_global_initialize_mp(struct state_conf *state)
{
	num_ports = state->source_port_last - state->source_port_first + 1;
	return EXIT_SUCCESS;
}

int synscan_init_perthread_mp(void* buf, macaddr_t *src,
		macaddr_t *gw, port_h_t dst_port,
		__attribute__((unused)) void **arg_ptr)
{
	memset(buf, 0, MAX_PACKET_SIZE);
	struct ether_header *eth_header = (struct ether_header *) buf;
	make_eth_header(eth_header, src, gw);
	struct ip *ip_header = (struct ip*)(&eth_header[1]);
	uint16_t len = htons(sizeof(struct ip) + sizeof(struct tcphdr) + MPTCP_SUB_LEN_CAPABLE_SYN); // also reserve space for MP_CAPABLE MP-TCP option
	make_ip_header(ip_header, IPPROTO_TCP, len);
	struct tcphdr *tcp_header = (struct tcphdr*)(&ip_header[1]);
	make_tcp_header(tcp_header, dst_port);
	tcp_header->th_off += MPTCP_SUB_LEN_CAPABLE_SYN / 4; // adjust data offset
	return EXIT_SUCCESS;
}

int synscan_make_mppacket(void *buf, ipaddr_n_t src_ip, ipaddr_n_t dst_ip,
		uint32_t *validation, int probe_num, __attribute__((unused)) void *arg)
{
	struct ether_header *eth_header = (struct ether_header *)buf;
	struct ip *ip_header = (struct ip*)(&eth_header[1]);
	struct tcphdr *tcp_header = (struct tcphdr*)(&ip_header[1]);
	struct mp_capable *mpc = (struct mp_capable*)(&tcp_header[1]);
	uint32_t tcp_seq = validation[0];

	ip_header->ip_src.s_addr = src_ip;
	ip_header->ip_dst.s_addr = dst_ip;

	tcp_header->th_sport = htons(get_src_port(num_ports,
				probe_num, validation));
	tcp_header->th_seq = tcp_seq;
	tcp_header->th_sum = 0;

	mpc->kind = TCPOPT_MPTCP;
	mpc->sender_key = 0xBEEFFEDBADC00FEE;
	mpc->len = MPTCP_SUB_LEN_CAPABLE_SYN;
	mpc->sub = MPTCP_SUB_CAPABLE;
	mpc->ver = 0;
	mpc->a = 0; /* Don't care about the checksum */
	mpc->b = 0;
	mpc->rsv = 0;
	mpc->h = 1;

	tcp_header->th_sum = tcp_checksum(sizeof(struct tcphdr) + MPTCP_SUB_LEN_CAPABLE_SYN,
			ip_header->ip_src.s_addr, ip_header->ip_dst.s_addr, tcp_header);

	ip_header->ip_sum = 0;
	ip_header->ip_sum = zmap_ip_checksum((unsigned short *) ip_header);

	return EXIT_SUCCESS;
}

void synscan_print_mppacket(FILE *fp, void* packet)
{
	struct ether_header *ethh = (struct ether_header *) packet;
	struct ip *iph = (struct ip *) &ethh[1];
	struct tcphdr *tcph = (struct tcphdr *) &iph[1];
	fprintf(fp, "tcp { source: %u | dest: %u | seq: %u | checksum: %#04X }\n",
			ntohs(tcph->th_sport),
			ntohs(tcph->th_dport),
			ntohl(tcph->th_seq),
			ntohs(tcph->th_sum));
	fprintf_ip_header(fp, iph);
	fprintf_eth_header(fp, ethh);
	fprintf(fp, "------------------------------------------------------\n");
}

int synscan_validate_mppacket(const struct ip *ip_hdr, uint32_t len,
		__attribute__((unused))uint32_t *src_ip,
		uint32_t *validation)
{
	if (ip_hdr->ip_p != IPPROTO_TCP) {
		return 0;
	}
	if ((4*ip_hdr->ip_hl + sizeof(struct tcphdr)) > len) {
		// buffer not large enough to contain expected tcp header
		return 0;
	}
	struct tcphdr *tcp = (struct tcphdr*)((char *) ip_hdr + 4*ip_hdr->ip_hl);
	uint16_t sport = tcp->th_sport;
	uint16_t dport = tcp->th_dport;
	// validate source port
	if (ntohs(sport) != zconf.target_port) {
		return 0;
	}
	// validate destination port
	if (!check_dst_port(ntohs(dport), num_ports, validation)) {
		return 0;
	}
	// validate tcp acknowledgement number
	if (htonl(tcp->th_ack) != htonl(validation[0])+1) {
		return 0;
	}
	return 1;
}

void synscan_process_mppacket(const u_char *packet,
		__attribute__((unused)) uint32_t len, fieldset_t *fs)
{
	struct ip *ip_hdr = (struct ip *)&packet[sizeof(struct ether_header)];
	struct tcphdr *tcp = (struct tcphdr*)((char *)ip_hdr
					+ 4*ip_hdr->ip_hl);

	fs_add_uint64(fs, "sport", (uint64_t) ntohs(tcp->th_sport));
	fs_add_uint64(fs, "dport", (uint64_t) ntohs(tcp->th_dport));
	fs_add_uint64(fs, "seqnum", (uint64_t) ntohl(tcp->th_seq));
	fs_add_uint64(fs, "acknum", (uint64_t) ntohl(tcp->th_ack));
	fs_add_uint64(fs, "window", (uint64_t) ntohs(tcp->th_win));

	if (tcp->th_flags & TH_RST) { // RST packet
		fs_add_string(fs, "classification", (char*) "rst", 0);
		fs_add_uint64(fs, "success", 0);
	} else { // SYNACK packet
		fs_add_string(fs, "classification", (char*) "synack", 0);
		fs_add_uint64(fs, "success", 1);
	}
}

static fielddef_t fields[] = {
	{.name = "sport",  .type = "int", .desc = "TCP source port"},
	{.name = "dport",  .type = "int", .desc = "TCP destination port"},
	{.name = "seqnum", .type = "int", .desc = "TCP sequence number"},
	{.name = "acknum", .type = "int", .desc = "TCP acknowledgement number"},
	{.name = "window", .type = "int", .desc = "TCP window"},
	{.name = "classification", .type="string", .desc = "packet classification"},
	{.name = "success", .type="int", .desc = "is response considered success"}
};

probe_module_t module_mptcp_synscan = {
	.name = "mptcp_synscan",
	.packet_length = 66,
	.pcap_filter = "tcp && tcp[13] & 4 != 0 || tcp[13] == 18",
	.pcap_snaplen = 96,
	.port_args = 1,
	.global_initialize = &synscan_global_initialize_mp,
	.thread_initialize = &synscan_init_perthread_mp,
	.make_packet = &synscan_make_mppacket,
	.print_packet = &synscan_print_mppacket,
	.process_packet = &synscan_process_mppacket,
	.validate_packet = &synscan_validate_mppacket,
	.close = NULL,

	.helptext = "Probe module that sends a TCP SYN packet with an MP_CAPABLE"
		"MP-TCP option to a specific port. Possible classifications are:"
		"synack and rst. A SYN-ACK packet is considered a success and a"
		"reset packet is considered a failed response.",

	.fields = fields,
	.numfields = 7};

