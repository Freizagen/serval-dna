/*
 Copyright (C) 2012 Serval Project.
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

/*
 
 Integration with olsr routing.
 
 - requires olsrd to be running on the same machine with plugin X loaded with the following configuration;
 LoadPlugin "name..."{
   PlParam  "BindPort"	"4130" 
   PlParam  "DestPort"	"4131"
   PlParam  "MagicNumber" "123"
 }
 
 This plugin will be used to forward broadcast mdp payloads to other instances of servald running on the network.

*/

#include "serval.h"
#include "overlay_packet.h"
#include "overlay_buffer.h"
#include "overlay_address.h"

#define PACKET_FORMAT_NUMBER 123
static int local_port =4131;
static int remote_port =4130;

static void olsr_read(struct sched_ent *alarm);

static struct profile_total read_timing={
  .name="olsr_read",
};

static struct sched_ent read_watch={
  .function=olsr_read,
  .stats=&read_timing,
  .poll.fd=-1,
  .poll.events=POLLIN,
};

int olsr_init_socket(void){
  int fd;
  int reuseP = 1;
  
  if (read_watch.poll.fd>=0)
    return 0;
  
  if (!confValueGetBoolean("olsr.enabled",0))
    return 0;
  
  local_port = confValueGetInt64Range("olsr.local.port", local_port, 1LL, 0xFFFFLL);
  remote_port = confValueGetInt64Range("olsr.remote.port", remote_port, 1LL, 0xFFFFLL);
  
  INFOF("Initialising olsr broadcast forwarding via ports %d-%d", local_port, remote_port);
  struct sockaddr_in addr = {
    .sin_family = AF_INET,
    .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    .sin_port = htons(local_port),
  };
  
  fd = socket(AF_INET,SOCK_DGRAM,0);
  if (fd < 0) {
    return WHY_perror("Error creating socket");
  } 
  
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseP, sizeof(reuseP)) < 0) {
    WHY_perror("setsockopt(SO_REUSEADR)");
    close(fd);
    return -1;
  }
  
#ifdef SO_REUSEPORT
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuseP, sizeof(reuseP)) < 0) {
    WHY_perror("setsockopt(SO_REUSEPORT)");
    close(fd);
    return -1;
  }
#endif
  
  /* Automatically close socket on calls to exec().
   This makes life easier when we restart with an exec after receiving
   a bad signal. */
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, NULL) |
#ifdef FD_CLOEXEC
 FD_CLOEXEC
#else
 O_CLOEXEC
#endif
);
  
  if (bind(fd, (struct sockaddr *)&addr, sizeof(struct sockaddr_in))) {
    WHY_perror("Bind failed");
    close(fd);
    return -1;
  }
  
  read_watch.poll.fd = fd;
  
  watch(&read_watch);
  return 0;
}

static void parse_frame(struct overlay_buffer *buff){
  struct overlay_frame frame;
  uint8_t addr_len;
  struct in_addr *addr;
  struct decode_context context={
    .please_explain=NULL,
  };
  
  memset(&frame,0,sizeof(struct overlay_frame));
  // parse the incoming olsr header
  int magic = ob_get(buff);
  if ((PACKET_FORMAT_NUMBER & 0xFF) != magic){
    WHYF("Unexpected magic number %d", magic);
    return;
  }
  overlay_address_clear();
  
  frame.ttl = ob_get(buff);
  addr_len = ob_get(buff);
  
  // Note IP6 not yet supported
  if (addr_len!=4)
    return;
  
  addr = (struct in_addr *)ob_get_bytes_ptr(buff, addr_len);
  
  // read subscriber id of transmitter
  struct subscriber *sender;
  if (overlay_address_parse(&context, buff, NULL, &sender))
    goto end;
  
  if (context.invalid_addresses)
    goto end;
  
  overlay_address_set_sender(sender);
  
  // locate the interface we should send outgoing unicast packets to
  overlay_interface *interface = overlay_interface_find(*addr);
  if (interface){
    // always update the IP address we heard them from, even if we don't need to use it right now
    sender->address.sin_family = AF_INET;
    sender->address.sin_addr = *addr;
    // assume the port number of the other servald matches our local port number configuration
    sender->address.sin_port = htons(interface->port);

    if (sender->reachable==REACHABLE_NONE){
      reachable_unicast(sender, interface, *addr, interface->port);
    }
  }
  
  // read subscriber id of payload origin
  if (overlay_address_parse(&context, buff, NULL, &frame.source))
    goto end;
  
  if (context.invalid_addresses)
    goto end;
  
  // read source broadcast id
  // assume each packet may arrive multiple times due to routing loops between servald overlay and olsr.
  if (overlay_address_parse(&context, buff, &frame.broadcast_id, NULL))
    goto end;
  
  if (context.invalid_addresses)
    goto end;
  
  frame.modifiers=ob_get(buff);
  
  if (debug&DEBUG_OVERLAYINTERFACES) 
    DEBUGF("Received %d byte payload via olsr", buff->sizeLimit - buff->position);
  
  // the remaining bytes are an mdp payload, process it
  frame.payload = buff;
  
  overlay_saw_mdp_containing_frame(&frame, gettime_ms());
  
  // TODO relay this packet to other non-olsr networks.
  
end:
  // if we didn't understand one of the address abreviations, ask for explanation
  send_please_explain(&context, my_subscriber, sender);
}

static void olsr_read(struct sched_ent *alarm){
  if (alarm->poll.revents & POLLIN) {
    unsigned char buff[1600];
    struct sockaddr_in addr;
    socklen_t size = sizeof(struct sockaddr_in);
    
    int msg_len = recvfrom(read_watch.poll.fd, buff, sizeof(buff), 0, (struct sockaddr *)&addr, &size);
    if (msg_len<3)
      return;
    
    // drop packets from other port numbers
    if (ntohs(addr.sin_port)!=remote_port){
      WHYF("Dropping unexpected packet from port %d", ntohs(addr.sin_port));
      return;
    }
    
    struct overlay_buffer *b = ob_static(buff, msg_len);
    ob_limitsize(b, msg_len);
    parse_frame(b);
    
    ob_free(b);
  }
  
  if (alarm->poll.revents & (POLLHUP | POLLERR)) {
    unwatch(alarm);
    close(alarm->poll.fd);
    alarm->poll.fd=-1;
    WHY("Olsr socket closed due to error");
  }
}

static int send_packet(unsigned char *header, int header_len, unsigned char *payload, int payload_len){
  struct sockaddr_in addr={
    .sin_family=AF_INET,
    .sin_addr.s_addr=htonl(INADDR_LOOPBACK),
    .sin_port=htons(remote_port),
  };
  
  struct iovec iov[]={
    {
      .iov_base=header,
      .iov_len=header_len,
    },
    {
      .iov_base=payload,
      .iov_len=payload_len,
    },
  };
  
  struct msghdr msg={
    .msg_name=&addr,
    .msg_namelen=sizeof(struct sockaddr_in),
    .msg_iov=iov,
    .msg_iovlen=2,
  };
  
  if (sendmsg(read_watch.poll.fd, &msg, 0)<0){
    return WHY_perror("Sending packet");
  }
  return 0;
}

int olsr_send(struct overlay_frame *frame){
  if (read_watch.poll.fd<0)
    return 0;
  // only send broadcasts
  if (frame->destination)
    return 0;
  
  struct overlay_buffer *b=ob_new();
  overlay_address_clear();
  
  // build olsr specific frame header
  ob_append_byte(b, PACKET_FORMAT_NUMBER);
  ob_append_byte(b, frame->ttl);
  
  // address the packet as transmitted by me
  overlay_address_append(b, my_subscriber);
  overlay_address_set_sender(my_subscriber);
  
  overlay_address_append(b, frame->source);
  overlay_broadcast_append(b, &frame->broadcast_id);
  ob_append_byte(b, frame->modifiers);
  
  if (debug&DEBUG_OVERLAYINTERFACES) 
    DEBUGF("Sending %d byte payload via olsr", frame->payload->sizeLimit);
  
  // send the packet
  int ret = send_packet(b->bytes, b->position, frame->payload->bytes, frame->payload->sizeLimit);
  ob_free(b);
  return ret;
}
