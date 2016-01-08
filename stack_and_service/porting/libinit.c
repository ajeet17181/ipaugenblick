/*
 * libinit.c
 *
 *  Created on: Jul 6, 2014
 *      Author: Vadim Suraev vadim.suraev@gmail.com
 *  Contains functions for initialization of
 *  Linux TCP/IP ported to userland and integrated with DPDK 1.6
 */
#define _GNU_SOURCE
#include<stdio.h>
#include <sys/param.h>
#include <rte_config.h>
#include <rte_ethdev.h>
#include <rte_memcpy.h>
#include <getopt.h>
#include <specific_includes/dpdk_drv_iface.h>
#include <pools.h>
#include <ipaugenblick_service_build.h>
#include <ipaugenblick_log.h>
#include <unistd.h>
#include <sched.h>

#define RTE_RX_DESC_DEFAULT (256)
#define RTE_TX_DESC_DEFAULT 256
#define RX_QUEUE_PER_PORT 1
#define TX_QUEUE_PER_PORT 1

//#define MBUFS_PER_RX_QUEUE (RTE_RX_DESC_DEFAULT*2/*+MAX_PKT_BURST*2*/)

//#define MBUFS_PER_TX_QUEUE /*((RTE_RX_DESC_DEFAULT+MAX_PKT_BURST*2)*RX_QUEUE_PER_PORT)*/4096*64
#define MBUF_SIZE (((2048 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM) + RTE_CACHE_LINE_SIZE) & ~(RTE_CACHE_LINE_SIZE))
#define MAX_PORTS	RTE_MAX_ETHPORTS
/*
 * RX and TX Prefetch, Host, and Write-back threshold values should be
 * carefully set for optimal performance. Consult the network
 * controller's datasheet and supporting DPDK documentation for guidance
 * on how these parameters should be set.
 */
#define RX_PTHRESH 8/*1*/ /**< Default values of RX prefetch threshold reg. */
#define RX_HTHRESH 8/*1*/ /**< Default values of RX host threshold reg. */
#define RX_WTHRESH 4/*1*/ /**< Default values of RX write-back threshold reg. */

/*
 * These default values are optimized for use with the Intel(R) 82599 10 GbE
 * Controller and the DPDK ixgbe PMD. Consider using other values for other
 * network controllers and/or network drivers.
 */
#define TX_PTHRESH 36/*0*/ /**< Default values of TX prefetch threshold reg. */
#define TX_HTHRESH 0  /**< Default values of TX host threshold reg. */
#define TX_WTHRESH 0  /**< Default values of TX write-back threshold reg. */

/*
 *
 * Configurable number of RX/TX ring descriptors
 */
static uint16_t nb_rxd = RTE_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TX_DESC_DEFAULT;
/* mask of enabled ports */
static uint32_t enabled_port_mask = 0;
static unsigned int rx_queue_per_lcore = 1;

struct port_statistics {
	uint64_t tx;
	uint64_t rx;
	uint64_t submitted_tx;
	uint64_t dropped;
	uint64_t tx_quota_exceeded;
	uint64_t tx_no_bufs;
} __rte_cache_aligned;
/*
 * This function may be called to calculate driver's optimal polling interval .
 * Paramters: a pointer to socket structure
 * Returns: None
 *
 */
int get_max_drv_poll_interval_in_micros(int port_num)
{
	struct rte_eth_link rte_eth_link;
	float bytes_in_sec,bursts_in_sec,bytes_in_burst;

	rte_eth_link_get(port_num,&rte_eth_link);
	switch(rte_eth_link.link_speed) {
	case ETH_LINK_SPEED_10:
		bytes_in_sec = 10/8;
		break;
	case ETH_LINK_SPEED_100:
		bytes_in_sec = 100/8;
		break;
	case ETH_LINK_SPEED_1000:
		bytes_in_sec = 1000/8;
		break;
	case ETH_LINK_SPEED_10000:
		bytes_in_sec = 10000/8;
		break;
	default:
		bytes_in_sec = 10000/8;
	}
	if(rte_eth_link.link_duplex == ETH_LINK_HALF_DUPLEX)
		bytes_in_sec /= 2;
	bytes_in_sec *= 1024*1024;/* x1M*/
	/*MTU*BURST_SIZE*/
	bytes_in_burst = 1448*MAX_PKT_BURST;
	bursts_in_sec = bytes_in_sec / bytes_in_burst;
	/* micros in sec div burst in sec = max poll interval in micros */
	return (int)(1000000/bursts_in_sec)/2/*safe side*/; /* casted to int, is not so large */
}

struct rte_mempool *pool_direct[RX_QUEUE_PER_PORT+1], *pool_indirect = NULL;

struct rte_mempool *get_direct_pool(uint16_t queue_id)
{
    if(queue_id < RX_QUEUE_PER_PORT) {
    	return pool_direct[queue_id];
    }
    ipaugenblick_log(IPAUGENBLICK_LOG_CRIT,"PANIC HERE %s %d\n",__FILE__,__LINE__);
    exit(1);
    return NULL;
}

/* This function initializes the rx queue rte_mbuf pool */
static void init_rx_queues_mempools(unsigned cpu, uint16_t queue_id)
{
        char pool_name[1024];

	ipaugenblick_log(IPAUGENBLICK_LOG_DEBUG,"CORE %d QUEUE %d\n", cpu, queue_id);
	sprintf(pool_name,"pool_direct%d",queue_id);
	pool_direct[queue_id] = rte_mempool_create(pool_name, MBUFS_PER_RX_QUEUE,
						   MBUF_SIZE, 0,
						   sizeof(struct rte_pktmbuf_pool_private),
						   rte_pktmbuf_pool_init, NULL,
						   rte_pktmbuf_init, NULL,
						   rte_lcore_to_socket_id(cpu), 0);
	if (pool_direct[queue_id] == NULL)
		rte_panic("Cannot init direct mbuf pool\n");
}

static struct rte_eth_conf port_conf = {
	.rxmode = {
		.split_hdr_size = 0,
		.header_split   = 0, /**< Header Split disabled */
		.hw_ip_checksum = 0, /**< IP checksum offload disabled */
		.hw_vlan_filter = 0, /**< VLAN filtering disabled */
		.jumbo_frame    = 0, /**< Jumbo Frame Support disabled */
		.hw_strip_crc   = 0, /**< CRC stripped by hardware */
		.mq_mode = ETH_MQ_RX_NONE/*ETH_MQ_RX_RSS*/,
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE/*ETH_MQ_TX_VMDQ_ONLY*/,
	},
};
#if 1
static const struct rte_eth_rxconf rx_conf = {
	.rx_thresh = {
		.pthresh = RX_PTHRESH,
		.hthresh = RX_HTHRESH,
		.wthresh = RX_WTHRESH,
	},
        .rx_free_thresh = MAX_PKT_BURST/*0*/,
        .rx_drop_en = 0,
};

static struct rte_eth_txconf tx_conf = {
	.tx_thresh = {
		.pthresh = TX_PTHRESH,
		.hthresh = TX_HTHRESH,
		.wthresh = TX_WTHRESH,
	},
	.tx_free_thresh = /*0*/MAX_PKT_BURST, /* Use PMD default values */
	.tx_rs_thresh = /*0*/MAX_PKT_BURST, /* Use PMD default values */
        .txq_flags = ((uint32_t)/*ETH_TXQ_FLAGS_NOMULTSEGS | \*/
			    ETH_TXQ_FLAGS_NOOFFLOADS),
};
#endif
static int parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	if (pm == 0)
		return -1;

	return pm;
}
#define DUMP(varname) printf("%s = %x\n", #varname, varname);
/* Parse the argument given in the command line of the application */
static int parse_args(int argc, char **argv)
{
	int opt, ret;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];
	static struct option lgopts[] = {
		{NULL, 0, 0, 0}
	};
DUMP(argc);
	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, "p:a:Tl:",
				  lgopts, &option_index)) != EOF) {

		switch (opt) {
		/* portmask */
		case 'p':
			enabled_port_mask = parse_portmask(optarg);
			if (enabled_port_mask == 0) {
				ipaugenblick_log(IPAUGENBLICK_LOG_ERR,"invalid portmask\n");
				//l2fwd_usage(prgname);
				return -1;
			}
			break;

		/* nqueue */
		case 'q':
			//l2fwd_rx_queue_per_lcore = l2fwd_parse_nqueue(optarg);
			//if (l2fwd_rx_queue_per_lcore == 0) {
				//printf("invalid queue number\n");
				//l2fwd_usage(prgname);
				//return -1;
			//}
			break;
		case 'l':
			ipaugenblick_set_log_level(atoi(optarg));
			break;
		default:
			//l2fwd_usage(prgname);
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 0; /* reset getopt lib */
	return ret;
}

static struct rte_mempool *mbufs_mempool = NULL;

extern unsigned long tcp_memory_allocated[MAXCPU];
extern uint64_t sk_stream_alloc_skb_failed;
extern uint64_t write_sockets_queue_len[MAXCPU];
extern uint64_t read_sockets_queue_len[MAXCPU];
extern struct rte_mempool *free_command_pool[MAXCPU];
extern volatile unsigned long long jiffies;
extern uint64_t driver_tx_offload_pkts;
extern uint64_t driver_tx_wo_offload_pkts;
void show_mib_stats(void);
/* This function only prints statistics, it it not called on data path */
static int print_stats(__attribute__((unused)) void *dummy)
{
	while(1) {
#if 1
		unsigned cpu;
		app_glue_print_stats();
		show_mib_stats();
		dpdk_dev_print_stats();
		print_user_stats();
		ipaugenblick_log(IPAUGENBLICK_LOG_INFO,"sk_stream_alloc_skb_failed %"PRIu64"\n",sk_stream_alloc_skb_failed);
		RTE_LCORE_FOREACH(cpu) {
			if(rte_lcore_is_enabled(cpu)) {
				if(rte_lcore_id() != cpu) {
					ipaugenblick_log(IPAUGENBLICK_LOG_INFO,"cpu#%d tcp_memory_allocated=%"PRIu64"\n",cpu,tcp_memory_allocated[cpu]);
					ipaugenblick_log(IPAUGENBLICK_LOG_INFO,"write_sockets_queue_len %"PRIu64" read_sockets_queue_len %"PRIu64" command pool %d \n",
						write_sockets_queue_len[cpu],read_sockets_queue_len[cpu],free_command_pool[cpu] ? rte_mempool_count(free_command_pool[cpu]) : -1);
				}
			}
		}
		ipaugenblick_log(IPAUGENBLICK_LOG_INFO,"jiffies %"PRIu64"\n",jiffies);
		dump_header_cache();
		dump_head_cache();
		dump_fclone_cache();
		ipaugenblick_log(IPAUGENBLICK_LOG_INFO,"rx pool free count %d\n",rte_mempool_count(pool_direct[0]));
		ipaugenblick_log(IPAUGENBLICK_LOG_INFO,"stack pool free count %d\n",rte_mempool_count(mbufs_mempool));	
		ipaugenblick_log(IPAUGENBLICK_LOG_INFO,"driver_tx_offload_pkts %"PRIu64" driver_tx_wo_offload_pkts %"PRIu64"\n",driver_tx_offload_pkts,driver_tx_wo_offload_pkts);
		print_skb_iov_stats();
#endif
		sleep(1);
	}
	return 0;
}
/* This function allocates rte_mbuf */
void *get_buffer()
{
	return rte_pktmbuf_alloc(mbufs_mempool);
}
/* this function gets a pointer to data in the newly allocated rte_mbuf */
void *get_data_ptr(void *buf)
{
	struct rte_mbuf *mbuf = (struct rte_mbuf *)buf;
	return rte_pktmbuf_mtod(mbuf,void *);
}
/* this function returns an available mbufs count */
int get_buffer_count()
{
	return rte_mempool_count(mbufs_mempool);
}
/* this function releases the rte_mbuf */
void release_buffer(void *buf)
{
	struct rte_mbuf *mbuf = (struct rte_mbuf *)buf;
	rte_pktmbuf_free_seg(mbuf);
}
typedef struct
{
	int  port_number;
	char ip_addr_str[20];
	char ip_mask_str[20];
}dpdk_dev_config_t;
#define ALIASES_MAX_NUMBER 4
static dpdk_dev_config_t dpdk_dev_config[RTE_MAX_ETHPORTS*ALIASES_MAX_NUMBER];
static void *dpdk_devices[RTE_MAX_ETHPORTS];
/* This function returns a pointer to kernel's interface structure, required to access the driver */
void *get_dpdk_dev_by_port_num(int port_num)
{
	if(port_num >= RTE_MAX_ETHPORTS) {
		return NULL;
	}
	return dpdk_devices[port_num];
}

extern char *get_dev_name(void *netdev);
#define IFACE_NAME_SIZE 20

static void encode_16(uint8_t *buf, uint16_t val)
{
	*buf = val << 8;
	buf++;
	*buf = val & 0xFF;
}
static void encode_32(uint8_t *buf, uint32_t val)
{
	*buf = val << 24;
	buf++;
	*buf = val << 16;
	buf++;
	*buf = val << 8;
	buf++;
	*buf = val;
}
static void encode_64(uint8_t *buf, uint64_t val)
{
	*buf = val << 56;
	buf++;
	*buf = val << 48;
	buf++;
	*buf = val << 40;
	buf++;
	*buf = val << 32;
	buf++;
	*buf = val << 24;
	buf++;
	*buf = val << 16;
	buf++;
	*buf = val << 8;
	buf++;
	*buf = val;
}

int get_all_addresses(unsigned char *buf)
{
    uint32_t dev_idx,offset = 0,prefix;
    uint8_t flags = (1<<0)|(1<<1)|(1<<6),family = 2 /*AF_INET*/, prefix_len = 32;

    for(dev_idx = 0;dev_idx < RTE_MAX_ETHPORTS;dev_idx++) {
	if((dpdk_devices[dev_idx])&&(dpdk_dev_config[dev_idx].port_number != -1)) {
		encode_32(&buf[offset],dev_idx);
		offset += sizeof(dev_idx);
		rte_memcpy(&buf[offset],&flags,sizeof(flags));
		offset += sizeof(flags);
		rte_memcpy(&buf[offset],&family,sizeof(family));
		offset += sizeof(family);	
		prefix = inet_addr(dpdk_dev_config[dev_idx].ip_addr_str);
		rte_memcpy(&buf[offset],&prefix,sizeof(prefix));
		offset += sizeof(prefix);
		rte_memcpy(&buf[offset],&prefix_len,sizeof(prefix_len));
		offset += sizeof(prefix_len);
		prefix = 0;
		rte_memcpy(&buf[offset],&prefix,sizeof(prefix));
		offset += sizeof(prefix);
		rte_memcpy(&buf[offset],&prefix_len,sizeof(prefix_len));
		offset += sizeof(prefix_len);
	}
    }
    return offset;
}

int get_all_devices(unsigned char *buf)
{
    uint32_t dev_idx,offset = 0,metric = 1,mtu = 1500,mtu6 = 1500,bandwidth = 0,hw_addr_len = 6;
    uint64_t flags = (1<<0)|(1<<1)|(1<<6);
    uint8_t status = 0,sockaddr_dl[6];

    for(dev_idx = 0;dev_idx < RTE_MAX_ETHPORTS;dev_idx++) {
	if(dpdk_devices[dev_idx]) {		
		rte_memcpy(&buf[offset],get_dev_name(dpdk_devices[dev_idx]),IFACE_NAME_SIZE);
		offset += IFACE_NAME_SIZE;
		encode_32(&buf[offset],dev_idx);
		offset += sizeof(dev_idx);	
		rte_memcpy(&buf[offset],&status,sizeof(status));
		offset += sizeof(status);
		encode_64(&buf[offset],flags);
		offset += sizeof(flags);
		encode_32(&buf[offset],metric);
		offset += sizeof(metric);
		encode_32(&buf[offset],mtu);
		offset += sizeof(mtu);
		encode_32(&buf[offset],mtu6);
		offset += sizeof(mtu6);
		encode_32(&buf[offset],bandwidth);
		offset += sizeof(bandwidth);
		hw_addr_len = htonl(hw_addr_len);
		rte_memcpy(&buf[offset],&hw_addr_len,sizeof(hw_addr_len));
		offset += sizeof(hw_addr_len);
		rte_memcpy(&buf[offset],sockaddr_dl,sizeof(sockaddr_dl));
		offset += sizeof(sockaddr_dl);
	}
    }
    return offset;
}
/* Helper function to read configuration file */
static int get_dpdk_ip_stack_config()
{
	FILE *p_config_file;
	int i,rc;
	p_config_file = fopen("/etc/ipaugenblick/dpdk_ip_stack_config.txt","r");
	if(!p_config_file){
		ipaugenblick_log(IPAUGENBLICK_LOG_ERR,"cannot open dpdk_ip_stack_config.txt");
		return -1;
	}
	for(i = 0;i < RTE_MAX_ETHPORTS*ALIASES_MAX_NUMBER;i++) {
		dpdk_dev_config[i].port_number = -1;
	}
	i = 0;
	while(!feof(p_config_file)) {
		rc = fscanf(p_config_file,"%d %s %s",&dpdk_dev_config[i].port_number,dpdk_dev_config[i].ip_addr_str,dpdk_dev_config[i].ip_mask_str);
		if(!rc){
			continue;
		}
		ipaugenblick_log(IPAUGENBLICK_LOG_DEBUG,"retrieved config entry %d %s %s\n",dpdk_dev_config[i].port_number,dpdk_dev_config[i].ip_addr_str,dpdk_dev_config[i].ip_mask_str);
		i++;
		if(i == RTE_MAX_ETHPORTS*ALIASES_MAX_NUMBER) {
			break;
		}
	}
	fclose(p_config_file);
	return 0;
}
static dpdk_dev_config_t *get_dpdk_config_entry(int portnum)
{
	int i;
	for(i = 0;i < RTE_MAX_ETHPORTS*ALIASES_MAX_NUMBER;i++) {
		if(dpdk_dev_config[i].port_number == portnum) {
			return &dpdk_dev_config[i];
		}
	}
	return NULL;
}
extern void ipaugenblick_main_loop(__attribute__((unused)) void *dummy);
static uint8_t nb_ports = 0;
static uint16_t queue_pairs = 0;
static uint16_t core_2_queue_idx[MAXCPU];
static struct rte_eth_dev_info dev_info[RTE_MAX_ETHPORTS];

void configure_netdevice_and_addresses(void)
{
	uint8_t portid;
	int sub_if_idx = 0;
	struct ether_addr mac_addr;
	dpdk_dev_config_t *p_dpdk_dev_config;

	for (portid = 0; portid < nb_ports; portid++) {
		/* skip ports that are not enabled */
		if ((enabled_port_mask & (1 << portid)) == 0)
			continue;
		p_dpdk_dev_config = get_dpdk_config_entry(portid);
		if(p_dpdk_dev_config) {	
			dpdk_devices[portid] = create_netdev(portid,dev_info[portid].tx_offload_capa);
			rte_eth_macaddr_get(portid,&mac_addr);
			set_dev_addr(dpdk_devices[portid],mac_addr.addr_bytes,p_dpdk_dev_config->ip_addr_str,p_dpdk_dev_config->ip_mask_str);
			sub_if_idx = portid;
			while(sub_if_idx < RTE_MAX_ETHPORTS*ALIASES_MAX_NUMBER) {
				p_dpdk_dev_config++;
				if(p_dpdk_dev_config->port_number != portid) {
					ipaugenblick_log(IPAUGENBLICK_LOG_DEBUG,"no more addressed for port %d\n",portid);
					break;
				}
				ipaugenblick_log(IPAUGENBLICK_LOG_DEBUG,"Adding port%d address %s\n",portid,p_dpdk_dev_config->ip_addr_str);
				add_dev_addr(dpdk_devices[portid],sub_if_idx - portid,p_dpdk_dev_config->ip_addr_str,p_dpdk_dev_config->ip_mask_str);
                	        sub_if_idx++;
			}
		}
	}
	if (nb_ports)
		return;
	portid = 0;
	while(1) {
		p_dpdk_dev_config = get_dpdk_config_entry(portid);
		if (!p_dpdk_dev_config) {
			break;
		}
		dpdk_devices[portid] = init_dpdk_sw_loop(portid);
		memset(mac_addr.addr_bytes,0,sizeof(mac_addr.addr_bytes));
		set_dev_addr(dpdk_devices[portid],mac_addr.addr_bytes,p_dpdk_dev_config->ip_addr_str,p_dpdk_dev_config->ip_mask_str);
		sub_if_idx = portid;
                while(sub_if_idx < RTE_MAX_ETHPORTS*ALIASES_MAX_NUMBER) {
                    p_dpdk_dev_config++;
                    if(p_dpdk_dev_config->port_number != portid) {
                        ipaugenblick_log(IPAUGENBLICK_LOG_DEBUG,"no more addressed for port %d\n",portid);
                        break;
                    }
                    ipaugenblick_log(IPAUGENBLICK_LOG_DEBUG,"Adding port%d address %s\n",portid,p_dpdk_dev_config->ip_addr_str);
                    add_dev_addr(dpdk_devices[portid],sub_if_idx - portid,p_dpdk_dev_config->ip_addr_str,p_dpdk_dev_config->ip_mask_str);
                    sub_if_idx++;
                }
		portid++;
	}
}

uint16_t get_queue_idx(void)
{
	return core_2_queue_idx[rte_lcore_id()];
}

static void adjust_queue_configuration()
{
	unsigned cpu;

	queue_pairs = 0;
	RTE_LCORE_FOREACH(cpu) {
		if(rte_lcore_is_enabled(cpu)) {
			if(rte_lcore_id() != cpu) {
				core_2_queue_idx[cpu] = queue_pairs;
				init_rx_queues_mempools(cpu, queue_pairs);
				queue_pairs++;
			}
		}
	}
}

static void configure_ports_queues(uint8_t portid)
{
	int ret;
	uint16_t queue_id;

	ipaugenblick_log(IPAUGENBLICK_LOG_DEBUG,"Port#%d queue pairs %d\n", portid, queue_pairs);

	ret = rte_eth_dev_configure(portid, queue_pairs, queue_pairs, &port_conf);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
				ret, (unsigned) portid);
	fflush(stdout);
	if (!dev_info[portid].tx_offload_capa) {
		nb_rxd = nb_txd = 256;
		tx_conf.txq_flags = ETH_TXQ_FLAGS_NOOFFLOADS;
	} else {
		nb_rxd = nb_txd = 4096;
		tx_conf.txq_flags = 0;
	}
	for(queue_id = 0; queue_id < queue_pairs; queue_id++) {
		ret = rte_eth_rx_queue_setup(portid, queue_id, nb_rxd,
				rte_eth_dev_socket_id(portid), &rx_conf,
				get_direct_pool(queue_id));
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
					ret, (unsigned) portid);
	}
	/* init one TX queue on each port */
	fflush(stdout);
	for(queue_id = 0; queue_id < queue_pairs; queue_id++) {
		ret = rte_eth_tx_queue_setup(portid, queue_id, nb_txd,
				rte_eth_dev_socket_id(portid), &tx_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
					ret, (unsigned) portid);
	}
}
/*
 * This function must be called prior any other in this package.
 * It initializes all the DPDK libs, reads the configuration, initializes the stack's
 * subsystems, allocates mbuf pools etc.
 * Parameters: refer to DPDK EAL parameters.
 * For example -c <core mask> -n <memory channels> -- -p <port mask>
 */
int dpdk_linux_tcpip_init(int argc,char **argv)
{
	int ret;
	uint8_t portid;
	unsigned cpu;

	ipaugenblick_log_init(0);
//	if(daemon(1,0)) {
//		printf("cannot daemonize\n");
//	}
	ipaugenblick_log(IPAUGENBLICK_LOG_INFO,"IPAugenblick service build  %s untracked_and_changed %s",IPAUGENBLICK_SERVICE_BUILD,IPAUGENBLICK_SERVICE_UNTRACKED_AND_CHANGED);	

	if(get_dpdk_ip_stack_config() != 0){
		ipaugenblick_log(IPAUGENBLICK_LOG_ERR,"cannot read configuration %s %d\n",__FILE__,__LINE__);
		return -1;
	}
	memset(dpdk_devices,0,sizeof(dpdk_devices));
	ret = rte_eal_init(argc, argv);
	if (ret < 0) {
		ipaugenblick_log(IPAUGENBLICK_LOG_ERR,"Invalid EAL arguments");
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	}
	init_lcores();
	rte_timer_subsystem_init();
	argc -= ret;
	argv += ret;
    	ret = parse_args(argc, argv);
	if (ret < 0) {
		ipaugenblick_log(IPAUGENBLICK_LOG_ERR,"Invalid APP arguments");
		rte_exit(EXIT_FAILURE, "Invalid APP arguments\n");
	}	
	
	mbufs_mempool = rte_mempool_create("mbufs_mempool", APP_MBUFS_POOL_SIZE,
							   MBUF_SIZE, 0,
							   sizeof(struct rte_pktmbuf_pool_private),
							   rte_pktmbuf_pool_init, NULL,
							   rte_pktmbuf_init, NULL,
							   rte_socket_id(), 0);
	if(mbufs_mempool == NULL) {
		ipaugenblick_log(IPAUGENBLICK_LOG_ERR,"cannot allocate buffers, increase the number of huge pages %s %d\n",__FILE__,__LINE__);
		exit(0);
	}
	rte_set_log_type(RTE_LOGTYPE_PMD,1);
	rte_set_log_level(RTE_LOG_DEBUG);
	nb_ports = rte_eth_dev_count();	

	if (nb_ports > RTE_MAX_ETHPORTS)
		nb_ports = RTE_MAX_ETHPORTS;
	if (nb_ports == 0)
		goto loopback_only;
	for (portid = 0; portid < nb_ports; portid++) {
		/* skip ports that are not enabled */
		if ((enabled_port_mask & (1 << portid)) == 0)
			continue;

		rte_eth_dev_info_get(portid, &dev_info[portid]);
	}      
	
	ipaugenblick_log(IPAUGENBLICK_LOG_DEBUG,"MASTER LCORE %d\n",rte_get_master_lcore());

	adjust_queue_configuration();
	/* Initialise each port */
	for (portid = 0; portid < nb_ports; portid++) {
		/* skip ports that are not enabled */
		if ((enabled_port_mask & (1 << portid)) == 0) {
			ipaugenblick_log(IPAUGENBLICK_LOG_WARNING,"Skipping disabled port %u\n", (unsigned) portid);
			continue;
		}
		/* init port */
		ipaugenblick_log(IPAUGENBLICK_LOG_DEBUG,"Initializing port %u... ", (unsigned) portid);
		fflush(stdout);
		configure_ports_queues(portid);
		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
					ret, (unsigned) portid);

		ipaugenblick_log(IPAUGENBLICK_LOG_DEBUG,"done: \n");

//		rte_eth_promiscuous_enable(portid);
	}
	rte_set_log_type(RTE_LOGTYPE_PMD,1);
	rte_set_log_level(RTE_LOG_DEBUG);	
loopback_only:
//	ipaugenblick_main_loop(NULL);
	RTE_LCORE_FOREACH(cpu) {
		if(rte_lcore_is_enabled(cpu)) {
#if 1
			if(rte_lcore_id() != cpu) {
				rte_eal_remote_launch(ipaugenblick_main_loop, NULL, cpu);
			}
#else
			if(rte_lcore_id() == cpu) {
				rte_eal_remote_launch(print_stats, NULL, cpu);
			}
#endif
		}
	}
	sleep(10);
#if 1
	print_stats(NULL);
#endif
	return 0;
}

