#include <inttypes.h>
#include <pcap.h>
#include <pthread.h>
#include <stdio.h>
/* exit() */
#include <stdlib.h>
/* strdup() */
#include <string.h>
/* time() */
#include <time.h>
/* sleep() */
#include <unistd.h>
/* update compression */
#include <zlib.h>
/* inet_ntoa() */
#include <arpa/inet.h>
/* DNS message header */
#include <arpa/nameser.h>
/* ETHER_HDR_LEN */
#include <net/ethernet.h>
/* IPPROTO_... */
#include <netinet/in.h>
/* struct ip */
#include <netinet/ip.h>
/* struct tcphdr */
#include <netinet/tcp.h>
/* struct udphdr */
#include <netinet/udp.h>

#ifndef DISABLE_ANONYMIZATION
#include "anonymization.h"
#endif
#include "dns_parser.h"
#include "dns_table.h"
#include "flow_table.h"
#include "mac_table.h"
#include "packet_series.h"

static packet_series_t packet_data;
static flow_table_t flow_table;
static dns_table_t dns_table;
static mac_table_t mac_table;

static pthread_t update_thread;
static pthread_mutex_t update_lock;

/* Will be filled in with the timestamp of the first packet pcap gives us. This
 * value serves as a unique identifier across instances of bismark-passive that
 * have run on the same machine. */
static int64_t first_packet_timestamp_microseconds = -1;

/* This extracts flow information from raw packet contents. */
static void get_flow_entry_for_packet(
    const u_char* const bytes,
    int len,
    flow_table_entry_t* const entry) {
  const struct ether_header* const eth_header = (struct ether_header*)bytes;
  if (ntohs(eth_header->ether_type) == ETHERTYPE_IP) {
    const struct iphdr* ip_header = (struct iphdr*)(bytes + ETHER_HDR_LEN);
    entry->ip_source = ntohl(ip_header->saddr);
    entry->ip_destination = ntohl(ip_header->daddr);
    entry->transport_protocol = ip_header->protocol;
    if (ip_header->protocol == IPPROTO_TCP) {
      const struct tcphdr* tcp_header = (struct tcphdr*)(
          (void *)ip_header + ip_header->ihl * sizeof(uint32_t));
      entry->port_source = ntohs(tcp_header->source);
      entry->port_destination = ntohs(tcp_header->dest);
    } else if (ip_header->protocol == IPPROTO_UDP) {
      const struct udphdr* udp_header = (struct udphdr*)(
          (void *)ip_header + ip_header->ihl * sizeof(uint32_t));
      entry->port_source = ntohs(udp_header->source);
      entry->port_destination = ntohs(udp_header->dest);

      if (entry->port_source == NS_DEFAULTPORT) {
        u_char* dns_bytes = (u_char*)udp_header + sizeof(struct udphdr);
        int dns_len = len - (dns_bytes - bytes);
        int mac_id = mac_table_lookup(&mac_table, eth_header->ether_dhost);
        process_dns_packet(dns_bytes, dns_len, &dns_table, mac_id);
      }
    } else {
#ifndef NDEBUG
      fprintf(stderr, "Unhandled transport protocol: %u\n", ip_header->protocol);
#endif
    }
  } else {
#ifndef NDEBUG
    fprintf(stderr, "Unhandled network protocol: %hu\n", ntohs(eth_header->ether_type));
#endif
  }
}

/* libpcap calls this function for every packet it receives. */
static void process_packet(
        u_char* const user,
        const struct pcap_pkthdr* const header,
        const u_char* const bytes) {
  if (pthread_mutex_lock(&update_lock)) {
    perror("Error locking global mutex");
    exit(1);
  }

#ifndef NDEBUG
  static int packets_received = 0;
  ++packets_received;
  if (packets_received % 1000 == 0) {
    pcap_t* const handle = (pcap_t*)user;
    struct pcap_stat statistics;
    pcap_stats(handle, &statistics);
    printf("-----\n");
    printf("STATISTICS (printed once for every thousand packets)\n");
    printf("Libpcap has dropped %d packets since process creation\n", statistics.ps_drop);
    printf("There are %d entries in the flow table\n", flow_table.num_elements);
    printf("The flow table has dropped %d flows\n", flow_table.num_dropped_flows);
    printf("The flow table has expired %d flows\n", flow_table.num_expired_flows);
    printf("-----\n");
  }
  if (packet_data.discarded_by_overflow % 1000 == 1) {
    printf("%d packets have overflowed the packet table!\n", packet_data.discarded_by_overflow);
  }
#endif

  if (first_packet_timestamp_microseconds < 0) {
    first_packet_timestamp_microseconds
        = header->ts.tv_sec * NUM_MICROS_PER_SECOND + header->ts.tv_usec;
  }

  flow_table_entry_t flow_entry;
  flow_table_entry_init(&flow_entry);
  get_flow_entry_for_packet(bytes, header->caplen, &flow_entry);
  int table_idx = flow_table_process_flow(
      &flow_table, &flow_entry, header->ts.tv_sec);
#ifndef NDEBUG
  if (table_idx < 0) {
    fprintf(stderr, "Error adding to flow table\n");
  }
#endif

  if (packet_series_add_packet(
        &packet_data, &header->ts, header->len, table_idx)) {
#ifndef NDEBUG
    fprintf(stderr, "Error adding to packet series\n");
#endif
  }

  if (pthread_mutex_unlock(&update_lock)) {
    perror("Error unlocking global mutex");
    exit(1);
  }
}

/* Write an update to UPDATE_FILENAME. This is the file that will be sent to the
 * server. The data is compressed on-the-fly using gzip. */
void write_update(const struct pcap_stat* statistics) {
  gzFile handle = gzopen (UPDATE_FILENAME, "wb");
  if (!handle) {
    perror("Could not open update file for writing");
    exit(1);
  }
  if (!gzprintf(handle,
                "%" PRId64 " %u %u %u\n\n",
                first_packet_timestamp_microseconds,
                statistics->ps_recv,
                statistics->ps_drop,
                statistics->ps_ifdrop)) {
    perror("Error writing update");
    exit(1);
  }
#ifndef DISABLE_ANONYMIZATION
  if (anonymization_write_update(handle)) {
    exit(1);
  }
#endif
  if (packet_series_write_update(&packet_data, handle)) {
    exit(1);
  }
  if (flow_table_write_update(&flow_table, handle)) {
    exit(1);
  }
  if (dns_table_write_update(&dns_table, handle)) {
    exit(1);
  }
  if (mac_table_write_update(&mac_table, handle)) {
    exit(1);
  }
  gzclose(handle);

  packet_series_init(&packet_data);
  flow_table_advance_base_timestamp(&flow_table, time(NULL));
  dns_table_destroy(&dns_table);
  dns_table_init(&dns_table);
}

void* updater(void* arg) {
  pcap_t* const handle = (pcap_t*)arg;
  while (1) {
    sleep (UPDATE_PERIOD_SECONDS);

    if (pthread_mutex_lock(&update_lock)) {
      perror("Error acquiring mutex for update");
      exit(1);
    }
#ifndef NDEBUG
    printf("Writing differential log to %s\n", UPDATE_FILENAME);
#endif
    struct pcap_stat statistics;
    if (!pcap_stats(handle, &statistics)) {
      write_update(&statistics);
    } else {
#ifndef NDEBUG
      pcap_perror(handle, "Error fetching pcap statistics");
#endif
      write_update(NULL);
    }
    if (pthread_mutex_unlock(&update_lock)) {
      perror("Error unlocking update mutex");
      exit(1);
    }
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <interface> [mac address 0] [mac address 1] ...\n", argv[0]);
    fprintf(stderr, "  if mac addresses are provided, only inspect packets to/from those addresses\n");
    return 1;
  }

  char* const dev = argv[1];
  char errbuf[PCAP_ERRBUF_SIZE];
  pcap_t* const handle = pcap_open_live(dev, BUFSIZ, 0, 1000, errbuf);
  if (!handle) {
    fprintf(stderr, "Couldn't open device %s: %s\n", dev, errbuf);
    return 2;
  }

  if (pcap_datalink(handle) != DLT_EN10MB) {
    fprintf(stderr, "Must capture on an Ethernet link\n");
    return 3;
  }

  if (argc >= 3) {
    char filter_program[256] = "";
    int idx;
    for (idx = 2; idx < argc; ++idx) {
      strncat(filter_program, "ether host ", sizeof(filter_program));
      strncat(filter_program, argv[idx], sizeof(filter_program));
      if (idx < argc - 1) {
        strncat(filter_program, " or ", sizeof(filter_program));
      }
    }
    struct bpf_program fp;
    if (pcap_compile(handle, &fp, filter_program, 0, PCAP_NETMASK_UNKNOWN)) {
      pcap_perror(handle, "Error creating packet filter");
      return 1;
    }
    if (pcap_setfilter(handle, &fp)) {
      pcap_perror(handle, "Error setting packet filter");
      return 1;
    }
  }

  if (pthread_mutex_init(&update_lock, NULL)) {
    perror("Error initializing mutex");
    return 4;
  }

#ifndef DISABLE_ANONYMIZATION
  if (anonymization_init()) {
    fprintf(stderr, "Error initializing anonymizer\n");
    return 1;
  }
#endif

  packet_series_init(&packet_data);
  flow_table_init(&flow_table);
  dns_table_init(&dns_table);
  mac_table_init(&mac_table);

  pthread_create(&update_thread, NULL, updater, handle);

  /* By default, pcap uses an internal buffer of 500 KB. Any packets that
   * overflow this buffer will be dropped. pcap_stats tells the number of
   * dropped packets.
   *
   * Because pcap does its own buffering, we don't need to run packet
   * processing in a separate thread. (It would be easier to just increase
   * the buffer size if we experience performance problems.) */
  return pcap_loop(handle, -1, process_packet, (u_char *)handle);
}