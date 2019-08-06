#include <Kernel/Lock.h>
#include <Kernel/Net/ARP.h>
#include <Kernel/Net/E1000NetworkAdapter.h>
#include <Kernel/Net/EtherType.h>
#include <Kernel/Net/EthernetFrameHeader.h>
#include <Kernel/Net/ICMP.h>
#include <Kernel/Net/IPv4.h>
#include <Kernel/Net/IPv4Socket.h>
#include <Kernel/Net/LoopbackAdapter.h>
#include <Kernel/Net/TCP.h>
#include <Kernel/Net/TCPSocket.h>
#include <Kernel/Net/UDP.h>
#include <Kernel/Net/UDPSocket.h>
#include <Kernel/Process.h>

//#define ETHERNET_DEBUG
//#define ETHERNET_VERY_DEBUG
//#define IPV4_DEBUG
//#define ICMP_DEBUG
//#define UDP_DEBUG
//#define TCP_DEBUG

static void handle_arp(const EthernetFrameHeader&, int frame_size);
static void handle_ipv4(const EthernetFrameHeader&, int frame_size);
static void handle_icmp(const EthernetFrameHeader&, int frame_size);
static void handle_udp(const EthernetFrameHeader&, int frame_size);
static void handle_tcp(const EthernetFrameHeader&, int frame_size);

Lockable<HashMap<IPv4Address, MACAddress>>& arp_table()
{
    static Lockable<HashMap<IPv4Address, MACAddress>>* the;
    if (!the)
        the = new Lockable<HashMap<IPv4Address, MACAddress>>;
    return *the;
}

void NetworkTask_main()
{
    LoopbackAdapter::the();

    auto* adapter = E1000NetworkAdapter::the();
    if (!adapter)
        dbgprintf("E1000 network card not found!\n");

    if (adapter)
        adapter->set_ipv4_address(IPv4Address(192, 168, 5, 2));

    auto dequeue_packet = [&]() -> Optional<KBuffer> {
        auto packet = LoopbackAdapter::the().dequeue_packet();
        if (packet.has_value()) {
            dbgprintf("Receive loopback packet (%d bytes)\n", packet.value().size());
            return packet.value();
        }
        if (adapter && adapter->has_queued_packets())
            return adapter->dequeue_packet();
        return {};
    };

    kprintf("NetworkTask: Enter main loop.\n");
    for (;;) {
        auto packet_maybe_null = dequeue_packet();
        if (!packet_maybe_null.has_value()) {
            (void)current->block_until("Networking", [] {
                if (LoopbackAdapter::the().has_queued_packets())
                    return true;
                if (auto* e1000 = E1000NetworkAdapter::the()) {
                    if (e1000->has_queued_packets())
                        return true;
                }
                return false;
            });
            continue;
        }
        auto& packet = packet_maybe_null.value();
        if (packet.size() < (int)(sizeof(EthernetFrameHeader))) {
            kprintf("NetworkTask: Packet is too small to be an Ethernet packet! (%d)\n", packet.size());
            continue;
        }
        auto& eth = *(const EthernetFrameHeader*)packet.data();
#ifdef ETHERNET_DEBUG
        kprintf("NetworkTask: From %s to %s, ether_type=%w, packet_length=%u\n",
            eth.source().to_string().characters(),
            eth.destination().to_string().characters(),
            eth.ether_type(),
            packet.size());
#endif

#ifdef ETHERNET_VERY_DEBUG
        u8* data = packet.data();

        for (size_t i = 0; i < packet.size(); i++) {
            kprintf("%b", data[i]);

            switch (i % 16) {
            case 7:
                kprintf("  ");
                break;
            case 15:
                kprintf("\n");
                break;
            default:
                kprintf(" ");
                break;
            }
        }

        kprintf("\n");
#endif

        switch (eth.ether_type()) {
        case EtherType::ARP:
            handle_arp(eth, packet.size());
            break;
        case EtherType::IPv4:
            handle_ipv4(eth, packet.size());
            break;
        }
    }
}

void handle_arp(const EthernetFrameHeader& eth, int frame_size)
{
    constexpr int minimum_arp_frame_size = sizeof(EthernetFrameHeader) + sizeof(ARPPacket);
    if (frame_size < minimum_arp_frame_size) {
        kprintf("handle_arp: Frame too small (%d, need %d)\n", frame_size, minimum_arp_frame_size);
        return;
    }
    auto& packet = *static_cast<const ARPPacket*>(eth.payload());
    if (packet.hardware_type() != 1 || packet.hardware_address_length() != sizeof(MACAddress)) {
        kprintf("handle_arp: Hardware type not ethernet (%w, len=%u)\n",
            packet.hardware_type(),
            packet.hardware_address_length());
        return;
    }
    if (packet.protocol_type() != EtherType::IPv4 || packet.protocol_address_length() != sizeof(IPv4Address)) {
        kprintf("handle_arp: Protocol type not IPv4 (%w, len=%u)\n",
            packet.hardware_type(),
            packet.protocol_address_length());
        return;
    }

#ifdef ARP_DEBUG
    kprintf("handle_arp: operation=%w, sender=%s/%s, target=%s/%s\n",
        packet.operation(),
        packet.sender_hardware_address().to_string().characters(),
        packet.sender_protocol_address().to_string().characters(),
        packet.target_hardware_address().to_string().characters(),
        packet.target_protocol_address().to_string().characters());
#endif

    if (packet.operation() == ARPOperation::Request) {
        // Who has this IP address?
        if (auto* adapter = NetworkAdapter::from_ipv4_address(packet.target_protocol_address())) {
            // We do!
            kprintf("handle_arp: Responding to ARP request for my IPv4 address (%s)\n",
                adapter->ipv4_address().to_string().characters());
            ARPPacket response;
            response.set_operation(ARPOperation::Response);
            response.set_target_hardware_address(packet.sender_hardware_address());
            response.set_target_protocol_address(packet.sender_protocol_address());
            response.set_sender_hardware_address(adapter->mac_address());
            response.set_sender_protocol_address(adapter->ipv4_address());

            adapter->send(packet.sender_hardware_address(), response);
        }
        return;
    }

    if (packet.operation() == ARPOperation::Response) {
        // Someone has this IPv4 address. I guess we can try to remember that.
        // FIXME: Protect against ARP spamming.
        // FIXME: Support static ARP table entries.
        LOCKER(arp_table().lock());
        arp_table().resource().set(packet.sender_protocol_address(), packet.sender_hardware_address());

        kprintf("ARP table (%d entries):\n", arp_table().resource().size());
        for (auto& it : arp_table().resource()) {
            kprintf("%s :: %s\n", it.value.to_string().characters(), it.key.to_string().characters());
        }
    }
}

void handle_ipv4(const EthernetFrameHeader& eth, int frame_size)
{
    constexpr int minimum_ipv4_frame_size = sizeof(EthernetFrameHeader) + sizeof(IPv4Packet);
    if (frame_size < minimum_ipv4_frame_size) {
        kprintf("handle_ipv4: Frame too small (%d, need %d)\n", frame_size, minimum_ipv4_frame_size);
        return;
    }
    auto& packet = *static_cast<const IPv4Packet*>(eth.payload());

#ifdef IPV4_DEBUG
    kprintf("handle_ipv4: source=%s, target=%s\n",
        packet.source().to_string().characters(),
        packet.destination().to_string().characters());
#endif

    switch ((IPv4Protocol)packet.protocol()) {
    case IPv4Protocol::ICMP:
        return handle_icmp(eth, frame_size);
    case IPv4Protocol::UDP:
        return handle_udp(eth, frame_size);
    case IPv4Protocol::TCP:
        return handle_tcp(eth, frame_size);
    default:
        kprintf("handle_ipv4: Unhandled protocol %u\n", packet.protocol());
        break;
    }
}

void handle_icmp(const EthernetFrameHeader& eth, int frame_size)
{
    (void)frame_size;
    auto& ipv4_packet = *static_cast<const IPv4Packet*>(eth.payload());
    auto& icmp_header = *static_cast<const ICMPHeader*>(ipv4_packet.payload());
#ifdef ICMP_DEBUG
    kprintf("handle_icmp: source=%s, destination=%s, type=%b, code=%b\n",
        ipv4_packet.source().to_string().characters(),
        ipv4_packet.destination().to_string().characters(),
        icmp_header.type(),
        icmp_header.code());
#endif

    {
        LOCKER(IPv4Socket::all_sockets().lock());
        for (RefPtr<IPv4Socket> socket : IPv4Socket::all_sockets().resource()) {
            LOCKER(socket->lock());
            if (socket->protocol() != (unsigned)IPv4Protocol::ICMP)
                continue;
            socket->did_receive(ipv4_packet.source(), 0, KBuffer::copy(&ipv4_packet, sizeof(IPv4Packet) + ipv4_packet.payload_size()));
        }
    }

    auto* adapter = NetworkAdapter::from_ipv4_address(ipv4_packet.destination());
    if (!adapter)
        return;

    if (icmp_header.type() == ICMPType::EchoRequest) {
        auto& request = reinterpret_cast<const ICMPEchoPacket&>(icmp_header);
        kprintf("handle_icmp: EchoRequest from %s: id=%u, seq=%u\n",
            ipv4_packet.source().to_string().characters(),
            (u16)request.identifier,
            (u16)request.sequence_number);
        size_t icmp_packet_size = ipv4_packet.payload_size();
        auto buffer = ByteBuffer::create_zeroed(icmp_packet_size);
        auto& response = *(ICMPEchoPacket*)buffer.pointer();
        response.header.set_type(ICMPType::EchoReply);
        response.header.set_code(0);
        response.identifier = request.identifier;
        response.sequence_number = request.sequence_number;
        if (size_t icmp_payload_size = icmp_packet_size - sizeof(ICMPEchoPacket))
            memcpy(response.payload(), request.payload(), icmp_payload_size);
        response.header.set_checksum(internet_checksum(&response, icmp_packet_size));
        adapter->send_ipv4(eth.source(), ipv4_packet.source(), IPv4Protocol::ICMP, buffer.data(), buffer.size());
    }
}

void handle_udp(const EthernetFrameHeader& eth, int frame_size)
{
    (void)frame_size;
    auto& ipv4_packet = *static_cast<const IPv4Packet*>(eth.payload());

    auto* adapter = NetworkAdapter::from_ipv4_address(ipv4_packet.destination());
    if (!adapter) {
        kprintf("handle_udp: this packet is not for me, it's for %s\n", ipv4_packet.destination().to_string().characters());
        return;
    }

    auto& udp_packet = *static_cast<const UDPPacket*>(ipv4_packet.payload());
#ifdef UDP_DEBUG
    kprintf("handle_udp: source=%s:%u, destination=%s:%u length=%u\n",
        ipv4_packet.source().to_string().characters(),
        udp_packet.source_port(),
        ipv4_packet.destination().to_string().characters(),
        udp_packet.destination_port(),
        udp_packet.length());
#endif

    auto socket = UDPSocket::from_port(udp_packet.destination_port());
    if (!socket) {
        kprintf("handle_udp: No UDP socket for port %u\n", udp_packet.destination_port());
        return;
    }

    ASSERT(socket->type() == SOCK_DGRAM);
    ASSERT(socket->local_port() == udp_packet.destination_port());
    socket->did_receive(ipv4_packet.source(), udp_packet.source_port(), KBuffer::copy(&ipv4_packet, sizeof(IPv4Packet) + ipv4_packet.payload_size()));
}

void handle_tcp(const EthernetFrameHeader& eth, int frame_size)
{
    (void)frame_size;
    auto& ipv4_packet = *static_cast<const IPv4Packet*>(eth.payload());

    auto* adapter = NetworkAdapter::from_ipv4_address(ipv4_packet.destination());
    if (!adapter) {
        kprintf("handle_tcp: this packet is not for me, it's for %s\n", ipv4_packet.destination().to_string().characters());
        return;
    }

    auto& tcp_packet = *static_cast<const TCPPacket*>(ipv4_packet.payload());
    size_t payload_size = ipv4_packet.payload_size() - tcp_packet.header_size();

#ifdef TCP_DEBUG
    kprintf("handle_tcp: source=%s:%u, destination=%s:%u seq_no=%u, ack_no=%u, flags=%w (%s%s%s%s), window_size=%u, payload_size=%u\n",
        ipv4_packet.source().to_string().characters(),
        tcp_packet.source_port(),
        ipv4_packet.destination().to_string().characters(),
        tcp_packet.destination_port(),
        tcp_packet.sequence_number(),
        tcp_packet.ack_number(),
        tcp_packet.flags(),
        tcp_packet.has_syn() ? "SYN " : "",
        tcp_packet.has_ack() ? "ACK " : "",
        tcp_packet.has_fin() ? "FIN " : "",
        tcp_packet.has_rst() ? "RST " : "",
        tcp_packet.window_size(),
        payload_size);
#endif

    IPv4SocketTuple tuple(ipv4_packet.destination(), tcp_packet.destination_port(), ipv4_packet.source(), tcp_packet.source_port());

    auto socket = TCPSocket::from_tuple(tuple);
    if (!socket) {
        kprintf("handle_tcp: No TCP socket for tuple %s\n", tuple.to_string().characters());
        return;
    }

    ASSERT(socket->type() == SOCK_STREAM);
    ASSERT(socket->local_port() == tcp_packet.destination_port());

    if (tcp_packet.ack_number() != socket->sequence_number()) {
        kprintf("handle_tcp: ack/seq mismatch: got %u, wanted %u\n", tcp_packet.ack_number(), socket->sequence_number());
        return;
    }

#ifdef TCP_DEBUG
    kprintf("handle_tcp: state=%s\n", TCPSocket::to_string(socket->state()));
#endif

    switch (socket->state()) {
    case TCPSocket::State::Closed:
        kprintf("handle_tcp: unexpected flags in Closed state\n");
        socket->send_tcp_packet(TCPFlags::RST);
        socket->set_state(TCPSocket::State::Closed);
        kprintf("handle_tcp: Closed -> Closed\n");
        return;
    case TCPSocket::State::TimeWait:
        kprintf("handle_tcp: unexpected flags in TimeWait state\n");
        socket->send_tcp_packet(TCPFlags::RST);
        socket->set_state(TCPSocket::State::Closed);
        kprintf("handle_tcp: TimeWait -> Closed\n");
        return;
    case TCPSocket::State::Listen:
        switch (tcp_packet.flags()) {
        case TCPFlags::SYN:
            kprintf("handle_tcp: incoming connections not supported\n");
            // socket->send_tcp_packet(TCPFlags::RST);
            return;
        default:
            kprintf("handle_tcp: unexpected flags in Listen state\n");
            // socket->send_tcp_packet(TCPFlags::RST);
            return;
        }
    case TCPSocket::State::SynSent:
        switch (tcp_packet.flags()) {
        case TCPFlags::SYN:
            socket->set_ack_number(tcp_packet.sequence_number() + payload_size + 1);
            socket->send_tcp_packet(TCPFlags::ACK);
            socket->set_state(TCPSocket::State::SynReceived);
            kprintf("handle_tcp: SynSent -> SynReceived\n");
            return;
        case TCPFlags::SYN | TCPFlags::ACK:
            socket->set_ack_number(tcp_packet.sequence_number() + payload_size + 1);
            socket->send_tcp_packet(TCPFlags::ACK);
            socket->set_state(TCPSocket::State::Established);
            socket->set_connected(true);
            kprintf("handle_tcp: SynSent -> Established\n");
            return;
        default:
            kprintf("handle_tcp: unexpected flags in SynSent state\n");
            socket->send_tcp_packet(TCPFlags::RST);
            socket->set_state(TCPSocket::State::Closed);
            kprintf("handle_tcp: SynSent -> Closed\n");
            return;
        }
    case TCPSocket::State::SynReceived:
        switch (tcp_packet.flags()) {
        case TCPFlags::ACK:
            socket->set_ack_number(tcp_packet.sequence_number() + payload_size + 1);
            socket->set_state(TCPSocket::State::Established);
            socket->set_connected(true);
            kprintf("handle_tcp: SynReceived -> Established\n");
            return;
        default:
            kprintf("handle_tcp: unexpected flags in SynReceived state\n");
            socket->send_tcp_packet(TCPFlags::RST);
            socket->set_state(TCPSocket::State::Closed);
            kprintf("handle_tcp: SynReceived -> Closed\n");
            return;
        }
    case TCPSocket::State::CloseWait:
        switch (tcp_packet.flags()) {
        default:
            kprintf("handle_tcp: unexpected flags in CloseWait state\n");
            socket->send_tcp_packet(TCPFlags::RST);
            socket->set_state(TCPSocket::State::Closed);
            kprintf("handle_tcp: CloseWait -> Closed\n");
            return;
        }
    case TCPSocket::State::LastAck:
        switch (tcp_packet.flags()) {
        case TCPFlags::ACK:
            socket->set_ack_number(tcp_packet.sequence_number() + payload_size + 1);
            socket->set_state(TCPSocket::State::Closed);
            kprintf("handle_tcp: LastAck -> Closed\n");
            return;
        default:
            kprintf("handle_tcp: unexpected flags in LastAck state\n");
            socket->send_tcp_packet(TCPFlags::RST);
            socket->set_state(TCPSocket::State::Closed);
            kprintf("handle_tcp: LastAck -> Closed\n");
            return;
        }
    case TCPSocket::State::FinWait1:
        switch (tcp_packet.flags()) {
        case TCPFlags::ACK:
            socket->set_ack_number(tcp_packet.sequence_number() + payload_size + 1);
            socket->set_state(TCPSocket::State::FinWait2);
            kprintf("handle_tcp: FinWait1 -> FinWait2\n");
            return;
        case TCPFlags::FIN:
            socket->set_ack_number(tcp_packet.sequence_number() + payload_size + 1);
            socket->set_state(TCPSocket::State::Closing);
            kprintf("handle_tcp: FinWait1 -> Closing\n");
            return;
        default:
            kprintf("handle_tcp: unexpected flags in FinWait1 state\n");
            socket->send_tcp_packet(TCPFlags::RST);
            socket->set_state(TCPSocket::State::Closed);
            kprintf("handle_tcp: FinWait1 -> Closed\n");
            return;
        }
    case TCPSocket::State::FinWait2:
        switch (tcp_packet.flags()) {
        case TCPFlags::FIN:
            socket->set_ack_number(tcp_packet.sequence_number() + payload_size + 1);
            socket->set_state(TCPSocket::State::TimeWait);
            kprintf("handle_tcp: FinWait2 -> TimeWait\n");
            return;
        default:
            kprintf("handle_tcp: unexpected flags in FinWait2 state\n");
            socket->send_tcp_packet(TCPFlags::RST);
            socket->set_state(TCPSocket::State::Closed);
            kprintf("handle_tcp: FinWait2 -> Closed\n");
            return;
        }
    case TCPSocket::State::Closing:
        switch (tcp_packet.flags()) {
        case TCPFlags::ACK:
            socket->set_ack_number(tcp_packet.sequence_number() + payload_size + 1);
            socket->set_state(TCPSocket::State::TimeWait);
            kprintf("handle_tcp: Closing -> TimeWait\n");
            return;
        default:
            kprintf("handle_tcp: unexpected flags in Closing state\n");
            socket->send_tcp_packet(TCPFlags::RST);
            socket->set_state(TCPSocket::State::Closed);
            kprintf("handle_tcp: Closing -> Closed\n");
            return;
        }
    case TCPSocket::State::Established:
        if (tcp_packet.has_fin()) {
            if (payload_size != 0)
                socket->did_receive(ipv4_packet.source(), tcp_packet.source_port(), KBuffer::copy(&ipv4_packet, sizeof(IPv4Packet) + ipv4_packet.payload_size()));

            socket->set_ack_number(tcp_packet.sequence_number() + payload_size + 1);
            socket->send_tcp_packet(TCPFlags::ACK);
            socket->set_state(TCPSocket::State::CloseWait);
            socket->set_connected(false);
            kprintf("handle_tcp: Established -> CloseWait\n");
            return;
        }

        socket->set_ack_number(tcp_packet.sequence_number() + payload_size);

#ifdef TCP_DEBUG
        kprintf("Got packet with ack_no=%u, seq_no=%u, payload_size=%u, acking it with new ack_no=%u, seq_no=%u\n",
            tcp_packet.ack_number(),
            tcp_packet.sequence_number(),
            payload_size,
            socket->ack_number(),
            socket->sequence_number());
#endif

        socket->send_tcp_packet(TCPFlags::ACK);

        if (payload_size != 0)
            socket->did_receive(ipv4_packet.source(), tcp_packet.source_port(), KBuffer::copy(&ipv4_packet, sizeof(IPv4Packet) + ipv4_packet.payload_size()));
    }
}
