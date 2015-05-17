/*
 * device.c
 *
 *  Created on: Jul 6, 2014
 *      Author: Vadim Suraev vadim.suraev@gmail.com
 *  Contains functions glueing ported Linux kernel's TCP/IP to the DPDK 1.6
 */
#include <specific_includes/dummies.h>
#include <specific_includes/linux/types.h>
#include <specific_includes/linux/bitops.h>
#include <specific_includes/linux/slab.h>
#include <specific_includes/linux/hash.h>
#include <specific_includes/linux/socket.h>
#include <specific_includes/linux/sockios.h>
#include <specific_includes/linux/if_ether.h>
#include <specific_includes/linux/netdevice.h>
#include <specific_includes/linux/etherdevice.h>
#include <specific_includes/linux/ethtool.h>
#include <specific_includes/linux/skbuff.h>
#include <specific_includes/net/net_namespace.h>
#include <specific_includes/net/sock.h>
#include <specific_includes/linux/rtnetlink.h>
#include <specific_includes/net/dst.h>
#include <specific_includes/net/checksum.h>
#include <specific_includes/linux/err.h>
#include <specific_includes/linux/if_arp.h>
#include <specific_includes/linux/if_vlan.h>
#include <specific_includes/linux/ip.h>
#include <specific_includes/net/ip.h>
#include <specific_includes/linux/ipv6.h>
#include <specific_includes/linux/in.h>
#include <specific_includes/linux/inetdevice.h>
#include <specific_includes/linux/hashtable.h>
#include <specific_includes/linux/if_macvlan.h>
#include <specific_includes/linux/if_arp.h>
#include <specific_includes/dpdk_drv_iface.h>
#include <specific_includes/linux/netdev_features.h>
#include <string.h>
#include <rte_config.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_timer.h>
#include <rte_byteorder.h>

typedef struct
{
	int port_number;
}dpdk_dev_priv_t;

/**
 * @internal Calculate a sum of all words in the buffer.
 * Helper routine for the rte_raw_cksum().
 *
 * @param buf
 *   Pointer to the buffer.
 * @param len
 *   Length of the buffer.
 * @param sum
 *   Initial value of the sum.
 * @return
 *   sum += Sum of all words in the buffer.
 */
static inline uint32_t
__rte_raw_cksum(const void *buf, size_t len, uint32_t sum)
{
        /* workaround gcc strict-aliasing warning */
        uintptr_t ptr = (uintptr_t)buf;
        const uint16_t *u16 = (const uint16_t *)ptr;

        while (len >= (sizeof(*u16) * 4)) {
                sum += u16[0];
                sum += u16[1];
                sum += u16[2];
                sum += u16[3];
                len -= sizeof(*u16) * 4;
                u16 += 4;
        }
        while (len >= sizeof(*u16)) {
                sum += *u16;
                len -= sizeof(*u16);
                u16 += 1;
        }

        /* if length is in odd bytes */
        if (len == 1)
                sum += *((const uint8_t *)u16);

        return sum;
}

/**
 * @internal Reduce a sum to the non-complemented checksum.
 * Helper routine for the rte_raw_cksum().
 *
 * @param sum
 *   Value of the sum.
 * @return
 *   The non-complemented checksum.
 */
static inline uint16_t
__rte_raw_cksum_reduce(uint32_t sum)
{
        sum = ((sum & 0xffff0000) >> 16) + (sum & 0xffff);
        sum = ((sum & 0xffff0000) >> 16) + (sum & 0xffff);
        return (uint16_t)sum;
}

/**
 * Process the non-complemented checksum of a buffer.
 *
 * @param buf
 *   Pointer to the buffer.
 * @param len
 *   Length of the buffer.
 * @return
 *   The non-complemented checksum.
 */
static inline uint16_t
rte_raw_cksum(const void *buf, size_t len)
{
        uint32_t sum;

        sum = __rte_raw_cksum(buf, len, 0);
        return __rte_raw_cksum_reduce(sum);
}

/* this function polls DPDK PMD driver for the received buffers.
 * It constructs skb and submits it to the stack.
 * netif_receive_skb is used, we don't have HW interrupt/BH contexts here
 */
static void rx_construct_skb_and_submit(struct net_device *netdev)
{
	int size = MAX_PKT_BURST,ret,i,frag_idx;
	struct rte_mbuf *mbufs[MAX_PKT_BURST],*m;
	struct sk_buff *skb;
        struct ethhdr *eth;
	dpdk_dev_priv_t *priv = netdev_priv(netdev);

	ret = dpdk_dev_get_received(priv->port_number,mbufs,size);
	if(unlikely(ret <= 0)) {
		return;
	}
        for(i = 0;i < ret;i++) {
            rte_prefetch0(rte_pktmbuf_mtod(mbufs[i],void *));
        }
	for(i = 0;i < ret;i++) {
		skb = build_skb(mbufs[i],rte_pktmbuf_data_len(mbufs[i]));
/*if(rte_pktmbuf_pkt_len(mbufs[i]) == 70)
{
	int j;
	unsigned char *p = rte_pktmbuf_mtod(mbufs[i], unsigned char *);
	printf("print 70 bytes\n");
	for(j = 0;j < rte_pktmbuf_data_len(mbufs[i]);j++)
		printf("  %x",p[j]);
}*/
		if(unlikely(skb == NULL)) {
			rte_pktmbuf_free(mbufs[i]);
			continue;
		}
		skb->len = rte_pktmbuf_data_len(mbufs[i]);
#if 0 /* once the receive will scatter the packets, this will be needed */
		m = mbufs[i]->pkt.next;
		frag_idx = 0;
		while(unlikely(m)) {
			struct page pg;

			pg.mbuf = m;
			skb_add_rx_frag(skb,frag_idx,&pg,0,m->pkt.data_len,m->pkt.data_len);
			frag_idx++;
			m = m->pkt.next;
		}
#endif
                /* removing vlan tagg */
                eth = (struct ethhdr *)skb->data;
                if(eth->h_proto == htons(ETH_P_8021Q)) {
                    unsigned i = 0;
                    uint8_t *hdr_str = (uint8_t *) skb->data;
                    for(i = 0; i < 12 ; i++)
                        hdr_str[(11 - i) + 4] = hdr_str[(11-i)];
                    skb->data = &hdr_str[4];
                    skb->len = skb->len - 4;
                }
		skb->protocol = eth_type_trans(skb, netdev);
		skb->ip_summed = CHECKSUM_UNNECESSARY;
		skb->dev = netdev;
		netif_receive_skb(skb);
	}
}

static int dpdk_open(struct net_device *netdev)
{
#ifdef DPDK_SW_LOOP
#endif
	return 0;
}
static int dpdk_close(struct net_device *netdev)
{
	return 0;
}
static netdev_tx_t dpdk_xmit_frame(struct sk_buff *skb,
                                  struct net_device *netdev)
{
	int i,pkt_len = 0;
	struct rte_mbuf **mbuf,*head;
	dpdk_dev_priv_t *priv = netdev_priv(netdev);
	skb_dst_force(skb);

	head = skb->header_mbuf;
	rte_pktmbuf_data_len(head) = skb_headroom(skb) + skb_headlen(skb);

	/* across all the stack, pkt.data in rte_mbuf is not moved while skb's data is.
	 * now is the time to do that
	 */
	rte_pktmbuf_adj(head,skb_headroom(skb));/* now mbuf data is at eth header */
	pkt_len = rte_pktmbuf_data_len(head);
	head->buf_addr = skb->data;

	mbuf = &head->next;
	skb->header_mbuf = NULL;
	head->nb_segs = 1 + skb_shinfo(skb)->nr_frags;
	/* typically, the headers are in skb->header_mbuf
	 * while the data is in the frags.
	 * An exception could be ICMP where skb->header_mbuf carries some payload aside headers
	 */
	for (i = 0; i < (int)skb_shinfo(skb)->nr_frags; i++) {
		*mbuf = skb_shinfo(skb)->frags[i].page.p;
		skb_frag_ref(skb,i);
		pkt_len += rte_pktmbuf_data_len((*mbuf));
		mbuf = &((*mbuf)->next);
	}
        *mbuf = NULL;
	rte_pktmbuf_pkt_len(head) = pkt_len;

#ifdef OFFLOAD_NOT_YET
       if ((skb->protocol == htons(ETH_P_IP))&&((ip_hdr(skb)->protocol == IPPROTO_TCP)||(ip_hdr(skb)->protocol == IPPROTO_UDP))) {
           head->ol_flags = PKT_TX_IP_CKSUM;
           struct iphdr *iph = ip_hdr(skb);
           iph->check = 0; 
           head->l3_len = /*sizeof(struct iphdr)*/skb_network_header_len(skb);
           head->l2_len = skb_network_offset(skb);
	   head->l4_len = 0;
	   head->tso_segsz = 0;
	   struct ipv4_psd_header {
	   	uint32_t src_addr; /* IP address of source host. */
                uint32_t dst_addr; /* IP address of destination host. */
		uint8_t  zero;     /* zero. */
                uint8_t  proto;    /* L4 protocol type. */
		uint16_t len;      /* L4 length. */
	    } psd_hdr;

	    psd_hdr.src_addr = iph->saddr;
	    psd_hdr.dst_addr = iph->daddr;
	    psd_hdr.zero = 0; 
if(skb_shinfo(skb)->nr_frags > 1) { printf("%s %d\n",__FILE__,__LINE__);exit(0); }
           if (/*(skb_shinfo(skb)->nr_frags)&&*/(ip_hdr(skb)->protocol == IPPROTO_TCP)) {
	       psd_hdr.proto = IPPROTO_TCP;  
	       head->tso_segsz =  skb_shinfo(skb)->gso_size;
	       head->l4_len = tcp_hdrlen(skb) /*+head->tso_segsz*/;
//printf("l3len %d l2len %d l4len %d tso %d %d %d %d\n",head->l3_len, head->l2_len, head->l4_len, head->tso_segsz,pkt_len,rte_pktmbuf_data_len(head),head->nb_segs);
	       if (head->tso_segsz) { /* this does not work */
			head->ol_flags |= PKT_TX_TCP_SEG;
			psd_hdr.len = 0;
	       }
	       else {
			head->ol_flags |= PKT_TX_TCP_CKSUM;
			psd_hdr.len = rte_cpu_to_be_16((uint16_t)(rte_be_to_cpu_16(iph->tot_len) - head->l3_len));
	       }
               tcp_hdr(skb)->check =  rte_raw_cksum(&psd_hdr, sizeof(psd_hdr));
	   }
           else if(ip_hdr(skb)->protocol == IPPROTO_UDP) {
	       psd_hdr.proto = IPPROTO_UDP;
	       head->l4_len = sizeof(struct udphdr);
               head->ol_flags |= PKT_TX_UDP_CKSUM;
	       psd_hdr.len = rte_cpu_to_be_16((uint16_t)(rte_be_to_cpu_16(iph->tot_len) - head->l3_len));
	       udp_hdr(skb)->check = rte_raw_cksum(&psd_hdr, sizeof(psd_hdr));
	   } 
       }
       else {
		printf("%s %d %x %lu %d\n",__FILE__,__LINE__,skb->protocol,head->ol_flags,rte_pktmbuf_data_len(head));
		{
		    unsigned char *p = rte_pktmbuf_mtod(head, unsigned char *);
		    for(i = 0;i < rte_pktmbuf_data_len(head);i++)
			printf("  %x",p[i]);
		}
	}
#else
	if(skb->protocol == htons(ETH_P_IP)) {
		head->ol_flags = PKT_TX_IP_CKSUM;
	        struct iphdr *iph = ip_hdr(skb);
        	iph->check = 0;
		head->l3_len = /*sizeof(struct iphdr)*/skb_network_header_len(skb);
           	head->l2_len = skb_network_offset(skb);
		if(ip_hdr(skb)->protocol == IPPROTO_TCP) {
			head->ol_flags |= PKT_TX_TCP_CKSUM;
		}
		else if(ip_hdr(skb)->protocol == IPPROTO_UDP) {
			head->ol_flags |= PKT_TX_UDP_CKSUM;
		}
	}
#endif
	/* this will pass the mbuf to DPDK PMD driver */
	dpdk_dev_enqueue_for_tx(priv->port_number,head);
	kfree_skb(skb);
	return NETDEV_TX_OK;
}
static struct rtnl_link_stats64 *dpdk_get_stats64(struct net_device *netdev,
                                                 struct rtnl_link_stats64 *stats)
{
	return NULL;
}
static void dpdk_set_rx_mode(struct net_device *netdev)
{
}
static int dpdk_set_mac(struct net_device *netdev, void *p)
{
	struct sockaddr *addr = p;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

    memcpy(netdev->dev_addr, addr->sa_data, netdev->addr_len);
	return 0;
}
static int dpdk_change_mtu(struct net_device *netdev, int new_mtu)
{
	return 0;
}
static int dpdk_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{
	return 0;
}
static void dpdk_tx_timeout(struct net_device *netdev)
{
}
static int dpdk_vlan_rx_add_vid(struct net_device *netdev,
                               __be16 proto, u16 vid)
{
	return 0;
}
static int dpdk_vlan_rx_kill_vid(struct net_device *netdev,
                                __be16 proto, u16 vid)
{
	return 0;
}
static int dpdk_ndo_set_vf_mac(struct net_device *netdev, int vf, u8 *mac)
{
	return 0;
}
static int dpdk_ndo_set_vf_vlan(struct net_device *netdev,
                               int vf, u16 vlan, u8 qos)
{
	return 0;
}
static int dpdk_ndo_set_vf_bw(struct net_device *netdev, int vf, int tx_rate)
{
	return 0;
}
static int dpdk_ndo_set_vf_spoofchk(struct net_device *netdev, int vf,
                                   bool setting)
{
	return 0;
}
static int dpdk_ndo_get_vf_config(struct net_device *netdev,
                                 int vf, struct ifla_vf_info *ivi)
{
	return 0;
}
static void dpdk_netpoll(struct net_device *netdev)
{
	dpdk_dev_priv_t *priv = netdev_priv(netdev);
	/* check for received packets.
	 * Then check if there are mbufs ready for tx, but not submitted yet
	 */
	rx_construct_skb_and_submit(netdev);
}
static netdev_features_t dpdk_fix_features(struct net_device *netdev,
         netdev_features_t features)
{
	netdev->features = features;
	return netdev->features;
}
static int dpdk_set_features(struct net_device *netdev,
         netdev_features_t features)
{
	netdev->features = features;
	return 0;
}
static const struct net_device_ops dpdk_netdev_ops = {
         .ndo_open               = dpdk_open,
         .ndo_stop               = dpdk_close,
         .ndo_start_xmit         = dpdk_xmit_frame,
         .ndo_get_stats64        = dpdk_get_stats64,
         .ndo_set_rx_mode        = dpdk_set_rx_mode,
         .ndo_set_mac_address    = dpdk_set_mac,
         .ndo_change_mtu         = dpdk_change_mtu,
         .ndo_do_ioctl           = dpdk_ioctl,
         .ndo_tx_timeout         = dpdk_tx_timeout,
         .ndo_validate_addr      = eth_validate_addr,
         .ndo_vlan_rx_add_vid    = dpdk_vlan_rx_add_vid,
         .ndo_vlan_rx_kill_vid   = dpdk_vlan_rx_kill_vid,
         .ndo_set_vf_mac         = dpdk_ndo_set_vf_mac,
         .ndo_set_vf_vlan        = dpdk_ndo_set_vf_vlan,
         .ndo_set_vf_tx_rate     = dpdk_ndo_set_vf_bw,
         .ndo_set_vf_spoofchk    = dpdk_ndo_set_vf_spoofchk,
         .ndo_get_vf_config      = dpdk_ndo_get_vf_config,
#ifdef CONFIG_NET_POLL_CONTROLLER
         .ndo_poll_controller    = dpdk_netpoll,
#endif
         .ndo_fix_features       = dpdk_fix_features,
         .ndo_set_features       = dpdk_set_features,
};
/* void user_transmitted_callback(struct rte_mbuf *mbuf,struct socket *sock)
{
        if(sock) {
               socket_satelite_data_t *socket_satelite_data = get_user_data(sock);
               if(socket_satelite_data) {
                       rte_atomic32_add(&g_ipaugenblick_sockets[socket_satelite_data->ringset_idx].tx_space,mbuf->pkt.data_len);
               }
        }
        rte_pktmbuf_free_seg(mbuf);
} */
char *get_dev_name(void *netdev)
{
	struct net_device *dev = (struct net_device *)netdev;
	return dev->name;
}
void add_dev_addr(void *netdev,int instance,char *ip_addr,char *ip_mask)
{
	struct socket *sock;
	struct ifreq ifr;
	struct rtentry rt;
	struct arpreq r;
	struct net_device *dev;
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
	int i;
	struct sockaddr macaddr;

	dev = (struct net_device *)netdev;
	if(netdev == NULL) {
		printf("netdev is NULL%s %d\n",__FILE__,__LINE__);
		goto leave;
	}
	memset(&ifr,0,sizeof(ifr));
	sprintf(ifr.ifr_ifrn.ifrn_name,"%s:%d",dev->name,instance);
	if(sock_create_kern(AF_INET,SOCK_STREAM,0,&sock)) {
		printf("cannot create socket %s %d\n",__FILE__,__LINE__);
		goto leave;
	}
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = inet_addr(ip_addr);
	if(inet_ioctl(sock,SIOCSIFADDR,&ifr)) {
		printf("Cannot set IF addr %s %d\n",__FILE__,__LINE__);
		goto leave;
	}
	memset(&ifr,0,sizeof(ifr));
	strcpy(ifr.ifr_ifrn.ifrn_name,dev->name);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = inet_addr(ip_mask);
	if(inet_ioctl(sock,SIOCSIFNETMASK,&ifr)) {
		printf("Cannot set IF mask %s %d\n",__FILE__,__LINE__);
		goto leave;
	}
	memset(&ifr,0,sizeof(ifr));
	sprintf(ifr.ifr_ifrn.ifrn_name,"%s:%d",dev->name,instance);
	ifr.ifr_flags |= IFF_UP;
	if(inet_ioctl(sock,SIOCSIFFLAGS,&ifr)) {
		printf("Cannot set IF flags %s %d\n",__FILE__,__LINE__);
		goto leave;
	}
	memset(&ifr,0,sizeof(ifr));
	sprintf(ifr.ifr_ifrn.ifrn_name,"%s:%d",dev->name,instance);
	if(inet_ioctl(sock,SIOCGIFADDR,&ifr)) {
		printf("Cannot get IF addr %s %d\n",__FILE__,__LINE__);
		goto leave;
	}
leave:
	kernel_close(sock);
}
void set_dev_addr(void *netdev,char *mac_addr,char *ip_addr,char *ip_mask)
{
	struct socket *sock;
	struct ifreq ifr;
	struct rtentry rt;
	struct arpreq r;
	struct net_device *dev;
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
	int i;
	struct sockaddr macaddr;

	dev = (struct net_device *)netdev;
	if(netdev == NULL) {
		printf("netdev is NULL%s %d\n",__FILE__,__LINE__);
		goto leave;
	}
        dev->mtu = 1500;
#ifdef OFFLOAD_NOT_YET
        dev->gso_max_segs = 2;
        dev->gso_max_size = 4096;
#endif
	memcpy(macaddr.sa_data,mac_addr,ETH_ALEN);
	memset(&ifr,0,sizeof(ifr));
	strcpy(ifr.ifr_ifrn.ifrn_name,dev->name);
	if(sock_create_kern(AF_INET,SOCK_STREAM,0,&sock)) {
		printf("cannot create socket %s %d\n",__FILE__,__LINE__);
		goto leave;
	}
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = inet_addr(ip_addr);
	if(inet_ioctl(sock,SIOCSIFADDR,&ifr)) {
		printf("Cannot set IF addr %s %d\n",__FILE__,__LINE__);
		goto leave;
	}
	memset(&ifr,0,sizeof(ifr));
	strcpy(ifr.ifr_ifrn.ifrn_name,dev->name);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = inet_addr(ip_mask);
	if(inet_ioctl(sock,SIOCSIFNETMASK,&ifr)) {
		printf("Cannot set IF mask %s %d\n",__FILE__,__LINE__);
		goto leave;
	}
	eth_mac_addr(dev,&macaddr);
	memset(&ifr,0,sizeof(ifr));
	strcpy(ifr.ifr_ifrn.ifrn_name,dev->name);
	ifr.ifr_flags |= IFF_UP;
	if(inet_ioctl(sock,SIOCSIFFLAGS,&ifr)) {
		printf("Cannot set IF flags %s %d\n",__FILE__,__LINE__);
		goto leave;
	}
	memset(&ifr,0,sizeof(ifr));
	strcpy(ifr.ifr_ifrn.ifrn_name,dev->name);
	if(inet_ioctl(sock,SIOCGIFADDR,&ifr)) {
		printf("Cannot get IF addr %s %d\n",__FILE__,__LINE__);
		goto leave;
	}
	IN_DEV_CONF_SET(in_dev_get(dev), FORWARDING,1);
leave:
	kernel_close(sock);
}

void *create_netdev(int port_num)
{
	struct net_device *netdev;
	dpdk_dev_priv_t *priv;
    char dev_name[20];

    sprintf(dev_name,"dpdk_if%d",port_num);
	netdev = alloc_netdev_mqs(sizeof(dpdk_dev_priv_t),dev_name,ether_setup,1,1);
	if(netdev == NULL) {
		printf("cannot allocate netdevice %s %d\n",__FILE__,__LINE__);
		return NULL;
	}
	priv = netdev_priv(netdev);
	memset(priv, 0, sizeof(dpdk_dev_priv_t));
	priv->port_number = port_num;
	netdev->netdev_ops = &dpdk_netdev_ops;
#ifdef OFFLOAD_NOT_YET
        netdev->features = NETIF_F_SG | NETIF_F_FRAGLIST|NETIF_F_V4_CSUM | NETIF_F_GSO;
#else
	netdev->features = NETIF_F_SG | NETIF_F_FRAGLIST | NETIF_F_V4_CSUM;
#endif
	netdev->hw_features = 0;

	netdev->vlan_features = 0;

	if(register_netdev(netdev)) {
		printf("Cannot register netdev %s %d\n",__FILE__,__LINE__);
		return NULL;
	}
	return netdev;
}
extern uint64_t received;
extern uint64_t transmitted;
extern uint64_t tx_dropped;
void dpdk_dev_print_stats()
{
	printf("PHY received %"PRIu64" transmitted %"PRIu64" dropped %"PRIu64"\n",received,transmitted,tx_dropped);
}
