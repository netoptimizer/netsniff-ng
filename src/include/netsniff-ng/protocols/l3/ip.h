/* XXX: Coding Style - use the tool indent with the following (Linux kernel
 *                     code indents)
 *
 * indent -nbad -bap -nbc -bbo -hnl -br -brs -c33 -cd33 -ncdb -ce -ci4   \
 *        -cli0 -d0 -di1 -nfc1 -i8 -ip0 -l120 -lp -npcs -nprs -npsl -sai \
 *        -saf -saw -ncs -nsc -sob -nfca -cp33 -ss -ts8 -il1
 *
 *
 * netsniff-ng
 *
 * High performance network sniffer for packet inspection
 *
 * Copyright (C) 2009, 2010  Daniel Borkmann <danborkmann@googlemail.com> and 
 *                           Emmanuel Roullit <emmanuel.roullit@googlemail.com>
 *
 * This program is free software; you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License as published by 
 * the Free Software Foundation; either version 2 of the License, or (at 
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY 
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License 
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 
 * 51 Franklin St, Fifth Floor, Boston, MA 02110, USA
 *
 * Note: Your kernel has to be compiled with CONFIG_PACKET_MMAP=y option in 
 *       order to use this.
 */

#ifndef	__PROTO_IP_H__
#define __PROTO_IP_H__

#include <stdint.h>
#include <assert.h>

#include <netinet/in.h>
#include <linux/ip.h>
#include <netsniff-ng/protocols/misc/csum.h>

#define	FRAG_OFF_RESERVED_FLAG(x) (x & 0x8000)
#define	FRAG_OFF_NO_FRAGMENT_FLAG(x) (x & 0x4000)
#define	FRAG_OFF_MORE_FRAGMENT_FLAG(x) (x & 0x2000)
#define	FRAG_OFF_FRAGMENT_OFFSET(x) (x & 0x1fff)

static inline struct iphdr *get_iphdr(uint8_t ** pkt, uint32_t * pkt_len)
{
	struct iphdr *ip_header;

	assert(pkt);
	assert(*pkt);
	assert(*pkt_len > sizeof(*ip_header));

	ip_header = (struct iphdr *)*pkt;

	*pkt += sizeof(*ip_header);
	*pkt_len -= sizeof(*ip_header);

	return (ip_header);
}

static inline uint16_t get_l4_type_from_ipv4(const struct iphdr *header)
{
	assert(header);
	return (header->protocol);
}

#endif				/* __PROTO_IP_H__ */