// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NET_H
#define BITCOIN_NET_H

#include <chainparams.h>
#include <common/bloom.h>
#include <compat.h>
#include <consensus/amount.h>
#include <crypto/siphash.h>
#include <hash.h>
#include <i2p.h>
#include <logging.h>
#include <net_permissions.h>
#include <netaddress.h>
#include <netbase.h>
#include <policy/feerate.h>
#include <protocol.h>
#include <random.h>
#include <span.h>
#include <streams.h>
#include <sync.h>
#include <threadinterrupt.h>
#include <uint256.h>
#include <util/check.h>
#include <util/sock.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

class AddrMan;
class BanMan;
class CNode;
class CScheduler;
struct bilingual_str;

/** Default for -whitelistrelay. */
static const bool DEFAULT_WHITELISTRELAY = true;
/** Default for -whitelistforcerelay. */
static const bool DEFAULT_WHITELISTFORCERELAY = false;

/** Time after which to disconnect, after waiting for a ping response (or inactivity). */
static constexpr std::chrono::minutes TIMEOUT_INTERVAL{20};
/** Run the feeler connection loop once every 2 minutes. **/
static constexpr auto FEELER_INTERVAL = 2min;
/** Run the extra block-relay-only connection loop once every 5 minutes. **/
static constexpr auto EXTRA_BLOCK_RELAY_ONLY_PEER_INTERVAL = 5min;
/** Maximum length of incoming protocol messages (no message over 4 MB is currently acceptable). */
static const unsigned int MAX_PROTOCOL_MESSAGE_LENGTH = 16 * 1000 * 1000;
/** Maximum length of the user agent string in `version` message */
static const unsigned int MAX_SUBVERSION_LENGTH = 256;
/** Maximum number of automatic outgoing nodes over which we'll relay everything (blocks, tx, addrs, etc) */
static const int MAX_OUTBOUND_FULL_RELAY_CONNECTIONS = 8;
/** Maximum number of addnode outgoing nodes */
static const int MAX_ADDNODE_CONNECTIONS = 8;
/** Maximum number of block-relay-only outgoing connections */
static const int MAX_BLOCK_RELAY_ONLY_CONNECTIONS = 2;
/** Maximum number of feeler connections */
static const int MAX_FEELER_CONNECTIONS = 1;
/** -listen default */
static const bool DEFAULT_LISTEN = true;
/** The maximum number of peer connections to maintain. */
static const unsigned int DEFAULT_MAX_PEER_CONNECTIONS = 125;
/** The default for -maxuploadtarget. 0 = Unlimited */
static const std::string DEFAULT_MAX_UPLOAD_TARGET{"0M"};
/** Default for blocks only*/
static const bool DEFAULT_BLOCKSONLY = false;
/** -peertimeout default */
static const int64_t DEFAULT_PEER_CONNECT_TIMEOUT = 60;
/** Number of file descriptors required for message capture **/
static const int NUM_FDS_MESSAGE_CAPTURE = 1;

static constexpr bool DEFAULT_FORCEDNSSEED{false};
static constexpr bool DEFAULT_DNSSEED{true};
static constexpr bool DEFAULT_FIXEDSEEDS{true};
static const size_t DEFAULT_MAXRECEIVEBUFFER = 5 * 1000;
static const size_t DEFAULT_MAXSENDBUFFER    = 1 * 1000;

typedef int64_t NodeId;

struct AddedNodeInfo
{
    std::string strAddedNode;
    CService resolvedAddress;
    bool fConnected;
    bool fInbound;
};

class CNodeStats;
class CClientUIInterface;

struct CSerializedNetMsg
{
    CSerializedNetMsg() = default;
    CSerializedNetMsg(CSerializedNetMsg&&) = default;
    CSerializedNetMsg& operator=(CSerializedNetMsg&&) = default;
    // No copying, only moves.
    CSerializedNetMsg(const CSerializedNetMsg& msg) = delete;
    CSerializedNetMsg& operator=(const CSerializedNetMsg&) = delete;

    std::vector<unsigned char> data;
    std::string m_type;
};

/** Different types of connections to a peer. This enum encapsulates the
 * information we have available at the time of opening or accepting the
 * connection. Aside from INBOUND, all types are initiated by us.
 *
 * If adding or removing types, please update CONNECTION_TYPE_DOC in
 * src/rpc/net.cpp and src/qt/rpcconsole.cpp, as well as the descriptions in
 * src/qt/guiutil.cpp and src/bitcoin-cli.cpp::NetinfoRequestHandler. */
enum class ConnectionType {
    /**
     * Inbound connections are those initiated by a peer. This is the only
     * property we know at the time of connection, until P2P messages are
     * exchanged.
     */
    INBOUND,

    /**
     * These are the default connections that we use to connect with the
     * network. There is no restriction on what is relayed; by default we relay
     * blocks, addresses & transactions. We automatically attempt to open
     * MAX_OUTBOUND_FULL_RELAY_CONNECTIONS using addresses from our AddrMan.
     */
    OUTBOUND_FULL_RELAY,


    /**
     * We open manual connections to addresses that users explicitly requested
     * via the addnode RPC or the -addnode/-connect configuration options. Even if a
     * manual connection is misbehaving, we do not automatically disconnect or
     * add it to our discouragement filter.
     */
    MANUAL,

    /**
     * Feeler connections are short-lived connections made to check that a node
     * is alive. They can be useful for:
     * - test-before-evict: if one of the peers is considered for eviction from
     *   our AddrMan because another peer is mapped to the same slot in the tried table,
     *   evict only if this longer-known peer is offline.
     * - move node addresses from New to Tried table, so that we have more
     *   connectable addresses in our AddrMan.
     * Note that in the literature ("Eclipse Attacks on Bitcoin’s Peer-to-Peer Network")
     * only the latter feature is referred to as "feeler connections",
     * although in our codebase feeler connections encompass test-before-evict as well.
     * We make these connections approximately every FEELER_INTERVAL:
     * first we resolve previously found collisions if they exist (test-before-evict),
     * otherwise we connect to a node from the new table.
     */
    FEELER,

    /**
     * We use block-relay-only connections to help prevent against partition
     * attacks. By not relaying transactions or addresses, these connections
     * are harder to detect by a third party, thus helping obfuscate the
     * network topology. We automatically attempt to open
     * MAX_BLOCK_RELAY_ONLY_ANCHORS using addresses from our anchors.dat. Then
     * addresses from our AddrMan if MAX_BLOCK_RELAY_ONLY_CONNECTIONS
     * isn't reached yet.
     */
    BLOCK_RELAY,

    /**
     * AddrFetch connections are short lived connections used to solicit
     * addresses from peers. These are initiated to addresses submitted via the
     * -seednode command line argument, or under certain conditions when the
     * AddrMan is empty.
     */
    ADDR_FETCH,
};

/** Convert ConnectionType enum to a string value */
std::string ConnectionTypeAsString(ConnectionType conn_type);

/**
 * Look up IP addresses from all interfaces on the machine and add them to the
 * list of local addresses to self-advertise.
 * The loopback interface is skipped and only the first address from each
 * interface is used.
 */
void Discover();

uint16_t GetListenPort();

enum
{
    LOCAL_NONE,   // unknown
    LOCAL_IF,     // address a local interface listens on
    LOCAL_BIND,   // address explicit bound to
    LOCAL_MAPPED, // address reported by UPnP or NAT-PMP
    LOCAL_MANUAL, // address explicitly specified (-externalip=)

    LOCAL_MAX
};

bool IsPeerAddrLocalGood(CNode *pnode);
/** Returns a local address that we should advertise to this peer */
std::optional<CAddress> GetLocalAddrForPeer(CNode *pnode);

/**
 * Mark a network as reachable or unreachable (no automatic connects to it)
 * @note Networks are reachable by default
 */
void SetReachable(enum Network net, bool reachable);
/** @returns true if the network is reachable, false otherwise */
bool IsReachable(enum Network net);
/** @returns true if the address is in a reachable network, false otherwise */
bool IsReachable(const CNetAddr& addr);

bool AddLocal(const CService& addr, int nScore = LOCAL_NONE);
bool AddLocal(const CNetAddr& addr, int nScore = LOCAL_NONE);
void RemoveLocal(const CService& addr);
bool SeenLocal(const CService& addr);
bool IsLocal(const CService& addr);
bool GetLocal(CService &addr, const CNetAddr *paddrPeer = nullptr);
CAddress GetLocalAddress(const CNetAddr *paddrPeer, ServiceFlags nLocalServices);


extern bool fDiscover;
extern bool fListen;

/** Subversion as sent to the P2P network in `version` messages */
extern std::string strSubVersion;

struct LocalServiceInfo {
    int nScore;
    uint16_t nPort;
};

extern Mutex g_maplocalhost_mutex;
extern std::map<CNetAddr, LocalServiceInfo> mapLocalHost GUARDED_BY(g_maplocalhost_mutex);

extern const std::string NET_MESSAGE_COMMAND_OTHER;
typedef std::map<std::string, uint64_t> mapMsgCmdSize; //command, total bytes

class CNodeStats
{
public:
    NodeId nodeid;
    ServiceFlags nServices;
    bool fRelayTxes;
    std::chrono::seconds m_last_send;
    std::chrono::seconds m_last_recv;
    std::chrono::seconds m_last_tx_time;
    std::chrono::seconds m_last_block_time;
    std::chrono::seconds m_connected;
    int64_t nTimeOffset;
    std::string m_addr_name;
    int nVersion;
    std::string cleanSubVer;
    bool fInbound;
    bool m_bip152_highbandwidth_to;
    bool m_bip152_highbandwidth_from;
    int m_starting_height;
    uint64_t nSendBytes;
    mapMsgCmdSize mapSendBytesPerMsgCmd;
    uint64_t nRecvBytes;
    mapMsgCmdSize mapRecvBytesPerMsgCmd;
    NetPermissionFlags m_permissionFlags;
    std::chrono::microseconds m_last_ping_time;
    std::chrono::microseconds m_min_ping_time;
    CAmount minFeeFilter;
    // Our address, as reported by the peer
    std::string addrLocal;
    // Address of this peer
    CAddress addr;
    // Bind address of our side of the connection
    CAddress addrBind;
    // Network the peer connected through
    Network m_network;
    uint32_t m_mapped_as;
    ConnectionType m_conn_type;
};


/** Transport protocol agnostic message container.
 * Ideally it should only contain receive time, payload,
 * type and size.
 */
class CNetMessage {
public:
    CDataStream m_recv;                  //!< received message data
    std::chrono::microseconds m_time{0}; //!< time of message receipt
    uint32_t m_message_size{0};          //!< size of the payload
    uint32_t m_raw_message_size{0};      //!< used wire size of the message (including header/checksum)
    std::string m_type;

    CNetMessage(CDataStream&& recv_in) : m_recv(std::move(recv_in)) {}

    void SetVersion(int nVersionIn)
    {
        m_recv.SetVersion(nVersionIn);
    }
};

/** The TransportDeserializer takes care of holding and deserializing the
 * network receive buffer. It can deserialize the network buffer into a
 * transport protocol agnostic CNetMessage (command & payload)
 */
class TransportDeserializer {
public:
    // returns true if the current deserialization is complete
    virtual bool Complete() const = 0;
    // set the serialization context version
    virtual void SetVersion(int version) = 0;
    /** read and deserialize data, advances msg_bytes data pointer */
    virtual int Read(Span<const uint8_t>& msg_bytes) = 0;
    // decomposes a message from the context
    virtual CNetMessage GetMessage(std::chrono::microseconds time, bool& reject_message) = 0;
    virtual ~TransportDeserializer() {}
};

class V1TransportDeserializer final : public TransportDeserializer
{
private:
    const CChainParams& m_chain_params;
    const NodeId m_node_id; // Only for logging
    mutable CHash256 hasher;
    mutable uint256 data_hash;
    bool in_data;                   // parsing header (false) or data (true)
    CDataStream hdrbuf;             // partially received header
    CMessageHeader hdr;             // complete header
    CDataStream vRecv;              // received message data
    unsigned int nHdrPos;
    unsigned int nDataPos;

    const uint256& GetMessageHash() const;
    int readHeader(Span<const uint8_t> msg_bytes);
    int readData(Span<const uint8_t> msg_bytes);

    void Reset() {
        vRecv.clear();
        hdrbuf.clear();
        hdrbuf.resize(24);
        in_data = false;
        nHdrPos = 0;
        nDataPos = 0;
        data_hash.SetNull();
        hasher.Reset();
    }

public:
    V1TransportDeserializer(const CChainParams& chain_params, const NodeId node_id, int nTypeIn, int nVersionIn)
        : m_chain_params(chain_params),
          m_node_id(node_id),
          hdrbuf(nTypeIn, nVersionIn),
          vRecv(nTypeIn, nVersionIn)
    {
        Reset();
    }

    bool Complete() const override
    {
        if (!in_data)
            return false;
        return (hdr.nMessageSize == nDataPos);
    }
    void SetVersion(int nVersionIn) override
    {
        hdrbuf.SetVersion(nVersionIn);
        vRecv.SetVersion(nVersionIn);
    }
    int Read(Span<const uint8_t>& msg_bytes) override
    {
        int ret = in_data ? readData(msg_bytes) : readHeader(msg_bytes);
        if (ret < 0) {
            Reset();
        } else {
            msg_bytes = msg_bytes.subspan(ret);
        }
        return ret;
    }
    CNetMessage GetMessage(std::chrono::microseconds time, bool& reject_message) override;
};

/** The TransportSerializer prepares messages for the network transport
 */
class TransportSerializer {
public:
    // prepare message for transport (header construction, error-correction computation, payload encryption, etc.)
    virtual void prepareForTransport(CSerializedNetMsg& msg, std::vector<unsigned char>& header) = 0;
    virtual ~TransportSerializer() {}
};

class V1TransportSerializer  : public TransportSerializer {
public:
    void prepareForTransport(CSerializedNetMsg& msg, std::vector<unsigned char>& header) override;
};

/** Information about a peer */
class CNode
{
    friend class CConnman;
    friend struct ConnmanTestMsg;

public:
    std::unique_ptr<TransportDeserializer> m_deserializer;
    std::unique_ptr<TransportSerializer> m_serializer;

    NetPermissionFlags m_permissionFlags{NetPermissionFlags::None};
    std::atomic<ServiceFlags> nServices{NODE_NONE};

    /**
     * Socket used for communication with the node.
     * May not own a Sock object (after `CloseSocketDisconnect()` or during tests).
     * `shared_ptr` (instead of `unique_ptr`) is used to avoid premature close of
     * the underlying file descriptor by one thread while another thread is
     * poll(2)-ing it for activity.
     * @see https://github.com/bitcoin/bitcoin/issues/21744 for details.
     */
    std::shared_ptr<Sock> m_sock GUARDED_BY(m_sock_mutex);

    /** Total size of all vSendMsg entries */
    size_t nSendSize GUARDED_BY(cs_vSend){0};
    /** Offset inside the first vSendMsg already sent */
    size_t nSendOffset GUARDED_BY(cs_vSend){0};
    uint64_t nSendBytes GUARDED_BY(cs_vSend){0};
    std::deque<std::vector<unsigned char>> vSendMsg GUARDED_BY(cs_vSend);
    Mutex cs_vSend;
    Mutex m_sock_mutex;
    Mutex cs_vRecv;

    RecursiveMutex cs_vProcessMsg;
    std::list<CNetMessage> vProcessMsg GUARDED_BY(cs_vProcessMsg);
    size_t nProcessQueueSize{0};

    RecursiveMutex cs_sendProcessing;

    uint64_t nRecvBytes GUARDED_BY(cs_vRecv){0};

    std::atomic<std::chrono::seconds> m_last_send{0s};
    std::atomic<std::chrono::seconds> m_last_recv{0s};
    //! Unix epoch time at peer connection
    const std::chrono::seconds m_connected;
    std::atomic<int64_t> nTimeOffset{0};
    // Address of this peer
    const CAddress addr;
    // Bind address of our side of the connection
    const CAddress addrBind;
    const std::string m_addr_name;
    //! Whether this peer is an inbound onion, i.e. connected via our Tor onion service.
    const bool m_inbound_onion;
    std::atomic<int> nVersion{0};
    Mutex m_subver_mutex;
    /**
     * cleanSubVer is a sanitized string of the user agent byte array we read
     * from the wire. This cleaned string can safely be logged or displayed.
     */
    std::string cleanSubVer GUARDED_BY(m_subver_mutex){};
    bool m_prefer_evict{false}; // This peer is preferred for eviction.
    bool HasPermission(NetPermissionFlags permission) const {
        return NetPermissions::HasFlag(m_permissionFlags, permission);
    }
    bool fClient{false}; // set by version message
    bool m_limited_node{false}; //after BIP159, set by version message
    /** fSuccessfullyConnected is set to true on receiving VERACK from the peer. */
    std::atomic_bool fSuccessfullyConnected{false};
    // Setting fDisconnect to true will cause the node to be disconnected the
    // next time DisconnectNodes() runs
    std::atomic_bool fDisconnect{false};
    CSemaphoreGrant grantOutbound;
    std::atomic<int> nRefCount{0};

    const uint64_t nKeyedNetGroup;
    std::atomic_bool fPauseRecv{false};
    std::atomic_bool fPauseSend{false};

    bool IsOutboundOrBlockRelayConn() const {
        switch (m_conn_type) {
            case ConnectionType::OUTBOUND_FULL_RELAY:
            case ConnectionType::BLOCK_RELAY:
                return true;
            case ConnectionType::INBOUND:
            case ConnectionType::MANUAL:
            case ConnectionType::ADDR_FETCH:
            case ConnectionType::FEELER:
                return false;
        } // no default case, so the compiler can warn about missing cases

        assert(false);
    }

    bool IsFullOutboundConn() const {
        return m_conn_type == ConnectionType::OUTBOUND_FULL_RELAY;
    }

    bool IsManualConn() const {
        return m_conn_type == ConnectionType::MANUAL;
    }

    bool IsBlockOnlyConn() const {
        return m_conn_type == ConnectionType::BLOCK_RELAY;
    }

    bool IsFeelerConn() const {
        return m_conn_type == ConnectionType::FEELER;
    }

    bool IsAddrFetchConn() const {
        return m_conn_type == ConnectionType::ADDR_FETCH;
    }

    bool IsInboundConn() const {
        return m_conn_type == ConnectionType::INBOUND;
    }

    bool ExpectServicesFromConn() const {
        switch (m_conn_type) {
            case ConnectionType::INBOUND:
            case ConnectionType::MANUAL:
            case ConnectionType::FEELER:
                return false;
            case ConnectionType::OUTBOUND_FULL_RELAY:
            case ConnectionType::BLOCK_RELAY:
            case ConnectionType::ADDR_FETCH:
                return true;
        } // no default case, so the compiler can warn about missing cases

        assert(false);
    }

    /**
     * Get network the peer connected through.
     *
     * Returns Network::NET_ONION for *inbound* onion connections,
     * and CNetAddr::GetNetClass() otherwise. The latter cannot be used directly
     * because it doesn't detect the former, and it's not the responsibility of
     * the CNetAddr class to know the actual network a peer is connected through.
     *
     * @return network the peer connected through.
     */
    Network ConnectedThroughNetwork() const;

    // We selected peer as (compact blocks) high-bandwidth peer (BIP152)
    std::atomic<bool> m_bip152_highbandwidth_to{false};
    // Peer selected us as (compact blocks) high-bandwidth peer (BIP152)
    std::atomic<bool> m_bip152_highbandwidth_from{false};

    struct TxRelay {
        mutable RecursiveMutex cs_filter;
        // We use fRelayTxes for two purposes -
        // a) it allows us to not relay tx invs before receiving the peer's version message
        // b) the peer may tell us in its version message that we should not relay tx invs
        //    unless it loads a bloom filter.
        bool fRelayTxes GUARDED_BY(cs_filter){false};
        std::unique_ptr<CBloomFilter> pfilter PT_GUARDED_BY(cs_filter) GUARDED_BY(cs_filter){nullptr};

        mutable RecursiveMutex cs_tx_inventory;
        CRollingBloomFilter filterInventoryKnown GUARDED_BY(cs_tx_inventory){50000, 0.000001};
        // Set of transaction ids we still have to announce.
        // They are sorted by the mempool before relay, so the order is not important.
        std::set<uint256> setInventoryTxToSend;
        // Used for BIP35 mempool sending
        bool fSendMempool GUARDED_BY(cs_tx_inventory){false};
        // Last time a "MEMPOOL" request was serviced.
        std::atomic<std::chrono::seconds> m_last_mempool_req{0s};
        std::chrono::microseconds nNextInvSend{0};

        /** Minimum fee rate with which to filter inv's to this node */
        std::atomic<CAmount> minFeeFilter{0};
        CAmount lastSentFeeFilter{0};
        std::chrono::microseconds m_next_send_feefilter{0};
    };

    // m_tx_relay == nullptr if we're not relaying transactions with this peer
    std::unique_ptr<TxRelay> m_tx_relay;

    /** UNIX epoch time of the last block received from this peer that we had
     * not yet seen (e.g. not already received from another peer), that passed
     * preliminary validity checks and was saved to disk, even if we don't
     * connect the block or it eventually fails connection. Used as an inbound
     * peer eviction criterium in CConnman::AttemptToEvictConnection. */
    std::atomic<std::chrono::seconds> m_last_block_time{0s};

    /** UNIX epoch time of the last transaction received from this peer that we
     * had not yet seen (e.g. not already received from another peer) and that
     * was accepted into our mempool. Used as an inbound peer eviction criterium
     * in CConnman::AttemptToEvictConnection. */
    std::atomic<std::chrono::seconds> m_last_tx_time{0s};

    /** Last measured round-trip time. Used only for RPC/GUI stats/debugging.*/
    std::atomic<std::chrono::microseconds> m_last_ping_time{0us};

    /** Lowest measured round-trip time. Used as an inbound peer eviction
     * criterium in CConnman::AttemptToEvictConnection. */
    std::atomic<std::chrono::microseconds> m_min_ping_time{std::chrono::microseconds::max()};

    CNode(NodeId id, ServiceFlags nLocalServicesIn, std::shared_ptr<Sock> sock, const CAddress& addrIn, uint64_t nKeyedNetGroupIn, uint64_t nLocalHostNonceIn, const CAddress& addrBindIn, const std::string& addrNameIn, ConnectionType conn_type_in, bool inbound_onion);
    CNode(const CNode&) = delete;
    CNode& operator=(const CNode&) = delete;

    NodeId GetId() const {
        return id;
    }

    uint64_t GetLocalNonce() const {
        return nLocalHostNonce;
    }

    int GetRefCount() const
    {
        assert(nRefCount >= 0);
        return nRefCount;
    }

    /**
     * Receive bytes from the buffer and deserialize them into messages.
     *
     * @param[in]   msg_bytes   The raw data
     * @param[out]  complete    Set True if at least one message has been
     *                          deserialized and is ready to be processed
     * @return  True if the peer should stay connected,
     *          False if the peer should be disconnected from.
     */
    bool ReceiveMsgBytes(Span<const uint8_t> msg_bytes, bool& complete);

    void SetCommonVersion(int greatest_common_version)
    {
        Assume(m_greatest_common_version == INIT_PROTO_VERSION);
        m_greatest_common_version = greatest_common_version;
    }
    int GetCommonVersion() const
    {
        return m_greatest_common_version;
    }

    CService GetAddrLocal() const LOCKS_EXCLUDED(m_addr_local_mutex);
    //! May not be called more than once
    void SetAddrLocal(const CService& addrLocalIn) LOCKS_EXCLUDED(m_addr_local_mutex);

    CNode* AddRef()
    {
        nRefCount++;
        return this;
    }

    void Release()
    {
        nRefCount--;
    }

    void AddKnownTx(const uint256& hash)
    {
        if (m_tx_relay != nullptr) {
            LOCK(m_tx_relay->cs_tx_inventory);
            m_tx_relay->filterInventoryKnown.insert(hash);
        }
    }

    void PushTxInventory(const uint256& hash)
    {
        if (m_tx_relay == nullptr) return;
        LOCK(m_tx_relay->cs_tx_inventory);
        if (!m_tx_relay->filterInventoryKnown.contains(hash)) {
            m_tx_relay->setInventoryTxToSend.insert(hash);
        }
    }

    void CloseSocketDisconnect();

    void CopyStats(CNodeStats& stats);

    ServiceFlags GetLocalServices() const
    {
        return nLocalServices;
    }

    std::string ConnectionTypeAsString() const { return ::ConnectionTypeAsString(m_conn_type); }

    /** A ping-pong round trip has completed successfully. Update latest and minimum ping times. */
    void PongReceived(std::chrono::microseconds ping_time) {
        m_last_ping_time = ping_time;
        m_min_ping_time = std::min(m_min_ping_time.load(), ping_time);
    }

private:
    const NodeId id;
    const uint64_t nLocalHostNonce;
    const ConnectionType m_conn_type;
    std::atomic<int> m_greatest_common_version{INIT_PROTO_VERSION};

    //! Services offered to this peer.
    //!
    //! This is supplied by the parent CConnman during peer connection
    //! (CConnman::ConnectNode()) from its attribute of the same name.
    //!
    //! This is const because there is no protocol defined for renegotiating
    //! services initially offered to a peer. The set of local services we
    //! offer should not change after initialization.
    //!
    //! An interesting example of this is NODE_NETWORK and initial block
    //! download: a node which starts up from scratch doesn't have any blocks
    //! to serve, but still advertises NODE_NETWORK because it will eventually
    //! fulfill this role after IBD completes. P2P code is written in such a
    //! way that it can gracefully handle peers who don't make good on their
    //! service advertisements.
    const ServiceFlags nLocalServices;

    std::list<CNetMessage> vRecvMsg; // Used only by SocketHandler thread

    // Our address, as reported by the peer
    CService addrLocal GUARDED_BY(m_addr_local_mutex);
    mutable Mutex m_addr_local_mutex;

    mapMsgCmdSize mapSendBytesPerMsgCmd GUARDED_BY(cs_vSend);
    mapMsgCmdSize mapRecvBytesPerMsgCmd GUARDED_BY(cs_vRecv);
};

/**
 * Interface for message handling
 */
class NetEventsInterface
{
public:
    /** Initialize a peer (setup state, queue any initial messages) */
    virtual void InitializeNode(const CNode* pnode) = 0;

    /** Handle removal of a peer (clear state) */
    virtual void FinalizeNode(const CNode& node) = 0;

    /**
    * Process protocol messages received from a given node
    *
    * @param[in]   pnode           The node which we have received messages from.
    * @param[in]   interrupt       Interrupt condition for processing threads
    * @return                      True if there is more work to be done
    */
    virtual bool ProcessMessages(CNode* pnode, std::atomic<bool>& interrupt) = 0;

    /**
    * Send queued protocol messages to a given node.
    *
    * @param[in]   pnode           The node which we are sending messages to.
    * @return                      True if there is more work to be done
    */
    virtual bool SendMessages(CNode* pnode) EXCLUSIVE_LOCKS_REQUIRED(pnode->cs_sendProcessing) = 0;


protected:
    /**
     * Protected destructor so that instances can only be deleted by derived classes.
     * If that restriction is no longer desired, this should be made public and virtual.
     */
    ~NetEventsInterface() = default;
};

class CConnman
{
public:

    struct Options
    {
        ServiceFlags nLocalServices = NODE_NONE;
        int nMaxConnections = 0;
        int m_max_outbound_full_relay = 0;
        int m_max_outbound_block_relay = 0;
        int nMaxAddnode = 0;
        int nMaxFeeler = 0;
        CClientUIInterface* uiInterface = nullptr;
        NetEventsInterface* m_msgproc = nullptr;
        BanMan* m_banman = nullptr;
        unsigned int nSendBufferMaxSize = 0;
        unsigned int nReceiveFloodSize = 0;
        uint64_t nMaxOutboundLimit = 0;
        int64_t m_peer_connect_timeout = DEFAULT_PEER_CONNECT_TIMEOUT;
        std::vector<std::string> vSeedNodes;
        std::vector<NetWhitelistPermissions> vWhitelistedRange;
        std::vector<NetWhitebindPermissions> vWhiteBinds;
        std::vector<CService> vBinds;
        std::vector<CService> onion_binds;
        /// True if the user did not specify -bind= or -whitebind= and thus
        /// we should bind on `0.0.0.0` (IPv4) and `::` (IPv6).
        bool bind_on_any;
        bool m_use_addrman_outgoing = true;
        std::vector<std::string> m_specified_outgoing;
        std::vector<std::string> m_added_nodes;
        bool m_i2p_accept_incoming;
    };

    void Init(const Options& connOptions) {
        nLocalServices = connOptions.nLocalServices;
        nMaxConnections = connOptions.nMaxConnections;
        m_max_outbound_full_relay = std::min(connOptions.m_max_outbound_full_relay, connOptions.nMaxConnections);
        m_max_outbound_block_relay = connOptions.m_max_outbound_block_relay;
        m_use_addrman_outgoing = connOptions.m_use_addrman_outgoing;
        nMaxAddnode = connOptions.nMaxAddnode;
        nMaxFeeler = connOptions.nMaxFeeler;
        m_max_outbound = m_max_outbound_full_relay + m_max_outbound_block_relay + nMaxFeeler;
        m_client_interface = connOptions.uiInterface;
        m_banman = connOptions.m_banman;
        m_msgproc = connOptions.m_msgproc;
        nSendBufferMaxSize = connOptions.nSendBufferMaxSize;
        nReceiveFloodSize = connOptions.nReceiveFloodSize;
        m_peer_connect_timeout = std::chrono::seconds{connOptions.m_peer_connect_timeout};
        {
            LOCK(cs_totalBytesSent);
            nMaxOutboundLimit = connOptions.nMaxOutboundLimit;
        }
        vWhitelistedRange = connOptions.vWhitelistedRange;
        {
            LOCK(m_added_nodes_mutex);
            m_added_nodes = connOptions.m_added_nodes;
        }
        m_onion_binds = connOptions.onion_binds;
    }

    CConnman(uint64_t seed0, uint64_t seed1, AddrMan& addrman, bool network_active = true);
    ~CConnman();
    bool Start(CScheduler& scheduler, const Options& options);

    void StopThreads();
    void StopNodes();
    void Stop()
    {
        StopThreads();
        StopNodes();
    };

    void Interrupt();
    bool GetNetworkActive() const { return fNetworkActive; };
    bool GetUseAddrmanOutgoing() const { return m_use_addrman_outgoing; };
    void SetNetworkActive(bool active);
    void OpenNetworkConnection(const CAddress& addrConnect, bool fCountFailure, CSemaphoreGrant* grantOutbound, const char* strDest, ConnectionType conn_type);
    bool CheckIncomingNonce(uint64_t nonce);

    bool ForNode(NodeId id, std::function<bool(CNode* pnode)> func);

    void PushMessage(CNode* pnode, CSerializedNetMsg&& msg);

    using NodeFn = std::function<void(CNode*)>;
    void ForEachNode(const NodeFn& func)
    {
        LOCK(m_nodes_mutex);
        for (auto&& node : m_nodes) {
            if (NodeFullyConnected(node))
                func(node);
        }
    };

    void ForEachNode(const NodeFn& func) const
    {
        LOCK(m_nodes_mutex);
        for (auto&& node : m_nodes) {
            if (NodeFullyConnected(node))
                func(node);
        }
    };

    // Addrman functions
    /**
     * Return all or many randomly selected addresses, optionally by network.
     *
     * @param[in] max_addresses  Maximum number of addresses to return (0 = all).
     * @param[in] max_pct        Maximum percentage of addresses to return (0 = all).
     * @param[in] network        Select only addresses of this network (nullopt = all).
     */
    std::vector<CAddress> GetAddresses(size_t max_addresses, size_t max_pct, std::optional<Network> network) const;
    /**
     * Cache is used to minimize topology leaks, so it should
     * be used for all non-trusted calls, for example, p2p.
     * A non-malicious call (from RPC or a peer with addr permission) should
     * call the function without a parameter to avoid using the cache.
     */
    std::vector<CAddress> GetAddresses(CNode& requestor, size_t max_addresses, size_t max_pct);

    // This allows temporarily exceeding m_max_outbound_full_relay, with the goal of finding
    // a peer that is better than all our current peers.
    void SetTryNewOutboundPeer(bool flag);
    bool GetTryNewOutboundPeer() const;

    void StartExtraBlockRelayPeers() {
        LogPrint(BCLog::NET, "net: enabling extra block-relay-only peers\n");
        m_start_extra_block_relay_peers = true;
    }

    // Return the number of outbound peers we have in excess of our target (eg,
    // if we previously called SetTryNewOutboundPeer(true), and have since set
    // to false, we may have extra peers that we wish to disconnect). This may
    // return a value less than (num_outbound_connections - num_outbound_slots)
    // in cases where some outbound connections are not yet fully connected, or
    // not yet fully disconnected.
    int GetExtraFullOutboundCount() const;
    // Count the number of block-relay-only peers we have over our limit.
    int GetExtraBlockRelayCount() const;

    bool AddNode(const std::string& node);
    bool RemoveAddedNode(const std::string& node);
    std::vector<AddedNodeInfo> GetAddedNodeInfo() const;

    /**
     * Attempts to open a connection. Currently only used from tests.
     *
     * @param[in]   address     Address of node to try connecting to
     * @param[in]   conn_type   ConnectionType::OUTBOUND, ConnectionType::BLOCK_RELAY,
     *                          ConnectionType::ADDR_FETCH or ConnectionType::FEELER
     * @return      bool        Returns false if there are no available
     *                          slots for this connection:
     *                          - conn_type not a supported ConnectionType
     *                          - Max total outbound connection capacity filled
     *                          - Max connection capacity for type is filled
     */
    bool AddConnection(const std::string& address, ConnectionType conn_type);

    size_t GetNodeCount(ConnectionDirection) const;
    void GetNodeStats(std::vector<CNodeStats>& vstats) const;
    bool DisconnectNode(const std::string& node);
    bool DisconnectNode(const CSubNet& subnet);
    bool DisconnectNode(const CNetAddr& addr);
    bool DisconnectNode(NodeId id);

    //! Used to convey which local services we are offering peers during node
    //! connection.
    //!
    //! The data returned by this is used in CNode construction,
    //! which is used to advertise which services we are offering
    //! that peer during `net_processing.cpp:PushNodeVersion()`.
    ServiceFlags GetLocalServices() const;

    uint64_t GetMaxOutboundTarget() const;
    std::chrono::seconds GetMaxOutboundTimeframe() const;

    //! check if the outbound target is reached
    //! if param historicalBlockServingLimit is set true, the function will
    //! response true if the limit for serving historical blocks has been reached
    bool OutboundTargetReached(bool historicalBlockServingLimit) const;

    //! response the bytes left in the current max outbound cycle
    //! in case of no limit, it will always response 0
    uint64_t GetOutboundTargetBytesLeft() const;

    //! returns the time left in the current max outbound cycle
    //! in case of no limit, it will always return 0
    std::chrono::seconds GetMaxOutboundTimeLeftInCycle() const;

    uint64_t GetTotalBytesRecv() const;
    uint64_t GetTotalBytesSent() const;

    /** Get a unique deterministic randomizer. */
    CSipHasher GetDeterministicRandomizer(uint64_t id) const;

    unsigned int GetReceiveFloodSize() const;

    void WakeMessageHandler();

    /** Return true if we should disconnect the peer for failing an inactivity check. */
    bool ShouldRunInactivityChecks(const CNode& node, std::chrono::seconds now) const;

private:
    struct ListenSocket {
    public:
        std::shared_ptr<Sock> sock;
        inline void AddSocketPermissionFlags(NetPermissionFlags& flags) const { NetPermissions::AddFlag(flags, m_permissions); }
        ListenSocket(std::shared_ptr<Sock> sock_, NetPermissionFlags permissions_)
            : sock{sock_}, m_permissions{permissions_}
        {
        }

    private:
        NetPermissionFlags m_permissions;
    };

    bool BindListenPort(const CService& bindAddr, bilingual_str& strError, NetPermissionFlags permissions);
    bool Bind(const CService& addr, unsigned int flags, NetPermissionFlags permissions);
    bool InitBinds(const Options& options);

    void ThreadOpenAddedConnections();
    void AddAddrFetch(const std::string& strDest);
    void ProcessAddrFetch();
    void ThreadOpenConnections(std::vector<std::string> connect);
    void ThreadMessageHandler();
    void ThreadI2PAcceptIncoming();
    void AcceptConnection(const ListenSocket& hListenSocket);

    /**
     * Create a `CNode` object from a socket that has just been accepted and add the node to
     * the `m_nodes` member.
     * @param[in] sock Connected socket to communicate with the peer.
     * @param[in] permissionFlags The peer's permissions.
     * @param[in] addr_bind The address and port at our side of the connection.
     * @param[in] addr The address and port at the peer's side of the connection.
     */
    void CreateNodeFromAcceptedSocket(std::unique_ptr<Sock>&& sock,
                                      NetPermissionFlags permissionFlags,
                                      const CAddress& addr_bind,
                                      const CAddress& addr);

    void DisconnectNodes();
    void NotifyNumConnectionsChanged();
    /** Return true if the peer is inactive and should be disconnected. */
    bool InactivityCheck(const CNode& node) const;

    /**
     * Generate a collection of sockets to check for IO readiness.
     * @param[in] nodes Select from these nodes' sockets.
     * @param[out] recv_set Sockets to check for read readiness.
     * @param[out] send_set Sockets to check for write readiness.
     * @param[out] error_set Sockets to check for errors.
     * @return true if at least one socket is to be checked (the returned set is not empty)
     */
    bool GenerateSelectSet(const std::vector<CNode*>& nodes,
                           std::set<SOCKET>& recv_set,
                           std::set<SOCKET>& send_set,
                           std::set<SOCKET>& error_set);

    /**
     * Check which sockets are ready for IO.
     * @param[in] nodes Select from these nodes' sockets.
     * @param[out] recv_set Sockets which are ready for read.
     * @param[out] send_set Sockets which are ready for write.
     * @param[out] error_set Sockets which have errors.
     * This calls `GenerateSelectSet()` to gather a list of sockets to check.
     */
    void SocketEvents(const std::vector<CNode*>& nodes,
                      std::set<SOCKET>& recv_set,
                      std::set<SOCKET>& send_set,
                      std::set<SOCKET>& error_set);

    /**
     * Check connected and listening sockets for IO readiness and process them accordingly.
     */
    void SocketHandler();

    /**
     * Do the read/write for connected sockets that are ready for IO.
     * @param[in] nodes Nodes to process. The socket of each node is checked against
     * `recv_set`, `send_set` and `error_set`.
     * @param[in] recv_set Sockets that are ready for read.
     * @param[in] send_set Sockets that are ready for send.
     * @param[in] error_set Sockets that have an exceptional condition (error).
     */
    void SocketHandlerConnected(const std::vector<CNode*>& nodes,
                                const std::set<SOCKET>& recv_set,
                                const std::set<SOCKET>& send_set,
                                const std::set<SOCKET>& error_set);

    /**
     * Accept incoming connections, one from each read-ready listening socket.
     * @param[in] recv_set Sockets that are ready for read.
     */
    void SocketHandlerListening(const std::set<SOCKET>& recv_set);

    void ThreadSocketHandler();
    void ThreadDNSAddressSeed();

    uint64_t CalculateKeyedNetGroup(const CAddress& ad) const;

    CNode* FindNode(const CNetAddr& ip);
    CNode* FindNode(const CSubNet& subNet);
    CNode* FindNode(const std::string& addrName);
    CNode* FindNode(const CService& addr);

    /**
     * Determine whether we're already connected to a given address, in order to
     * avoid initiating duplicate connections.
     */
    bool AlreadyConnectedToAddress(const CAddress& addr);

    bool AttemptToEvictConnection();
    CNode* ConnectNode(CAddress addrConnect, const char *pszDest, bool fCountFailure, ConnectionType conn_type);
    void AddWhitelistPermissionFlags(NetPermissionFlags& flags, const CNetAddr &addr) const;

    void DeleteNode(CNode* pnode);

    NodeId GetNewNodeId();

    size_t SocketSendData(CNode& node) const EXCLUSIVE_LOCKS_REQUIRED(node.cs_vSend);
    void DumpAddresses();

    // Network stats
    void RecordBytesRecv(uint64_t bytes);
    void RecordBytesSent(uint64_t bytes);

    /**
     * Return vector of current BLOCK_RELAY peers.
     */
    std::vector<CAddress> GetCurrentBlockRelayOnlyConns() const;

    // Whether the node should be passed out in ForEach* callbacks
    static bool NodeFullyConnected(const CNode* pnode);

    // Network usage totals
    mutable RecursiveMutex cs_totalBytesSent;
    std::atomic<uint64_t> nTotalBytesRecv{0};
    uint64_t nTotalBytesSent GUARDED_BY(cs_totalBytesSent) {0};

    // outbound limit & stats
    uint64_t nMaxOutboundTotalBytesSentInCycle GUARDED_BY(cs_totalBytesSent) {0};
    std::chrono::seconds nMaxOutboundCycleStartTime GUARDED_BY(cs_totalBytesSent) {0};
    uint64_t nMaxOutboundLimit GUARDED_BY(cs_totalBytesSent);

    // P2P timeout in seconds
    std::chrono::seconds m_peer_connect_timeout;

    // Whitelisted ranges. Any node connecting from these is automatically
    // whitelisted (as well as those connecting to whitelisted binds).
    std::vector<NetWhitelistPermissions> vWhitelistedRange;

    unsigned int nSendBufferMaxSize{0};
    unsigned int nReceiveFloodSize{0};

    std::vector<ListenSocket> vhListenSocket;
    std::atomic<bool> fNetworkActive{true};
    bool fAddressesInitialized{false};
    AddrMan& addrman;
    std::deque<std::string> m_addr_fetches GUARDED_BY(m_addr_fetches_mutex);
    Mutex m_addr_fetches_mutex;
    std::vector<std::string> m_added_nodes GUARDED_BY(m_added_nodes_mutex);
    mutable Mutex m_added_nodes_mutex;
    std::vector<CNode*> m_nodes GUARDED_BY(m_nodes_mutex);
    std::list<CNode*> m_nodes_disconnected;
    mutable RecursiveMutex m_nodes_mutex;
    std::atomic<NodeId> nLastNodeId{0};
    unsigned int nPrevNodeCount{0};

    /**
     * Cache responses to addr requests to minimize privacy leak.
     * Attack example: scraping addrs in real-time may allow an attacker
     * to infer new connections of the victim by detecting new records
     * with fresh timestamps (per self-announcement).
     */
    struct CachedAddrResponse {
        std::vector<CAddress> m_addrs_response_cache;
        std::chrono::microseconds m_cache_entry_expiration{0};
    };

    /**
     * Addr responses stored in different caches
     * per (network, local socket) prevent cross-network node identification.
     * If a node for example is multi-homed under Tor and IPv6,
     * a single cache (or no cache at all) would let an attacker
     * to easily detect that it is the same node by comparing responses.
     * Indexing by local socket prevents leakage when a node has multiple
     * listening addresses on the same network.
     *
     * The used memory equals to 1000 CAddress records (or around 40 bytes) per
     * distinct Network (up to 5) we have/had an inbound peer from,
     * resulting in at most ~196 KB. Every separate local socket may
     * add up to ~196 KB extra.
     */
    std::map<uint64_t, CachedAddrResponse> m_addr_response_caches;

    /**
     * Services this instance offers.
     *
     * This data is replicated in each CNode instance we create during peer
     * connection (in ConnectNode()) under a member also called
     * nLocalServices.
     *
     * This data is not marked const, but after being set it should not
     * change. See the note in CNode::nLocalServices documentation.
     *
     * \sa CNode::nLocalServices
     */
    ServiceFlags nLocalServices;

    std::unique_ptr<CSemaphore> semOutbound;
    std::unique_ptr<CSemaphore> semAddnode;
    int nMaxConnections;

    // How many full-relay (tx, block, addr) outbound peers we want
    int m_max_outbound_full_relay;

    // How many block-relay only outbound peers we want
    // We do not relay tx or addr messages with these peers
    int m_max_outbound_block_relay;

    int nMaxAddnode;
    int nMaxFeeler;
    int m_max_outbound;
    bool m_use_addrman_outgoing;
    CClientUIInterface* m_client_interface;
    NetEventsInterface* m_msgproc;
    /** Pointer to this node's banman. May be nullptr - check existence before dereferencing. */
    BanMan* m_banman;

    /**
     * Addresses that were saved during the previous clean shutdown. We'll
     * attempt to make block-relay-only connections to them.
     */
    std::vector<CAddress> m_anchors;

    /** SipHasher seeds for deterministic randomness */
    const uint64_t nSeed0, nSeed1;

    /** flag for waking the message processor. */
    bool fMsgProcWake GUARDED_BY(mutexMsgProc);

    std::condition_variable condMsgProc;
    Mutex mutexMsgProc;
    std::atomic<bool> flagInterruptMsgProc{false};

    /**
     * This is signaled when network activity should cease.
     * A pointer to it is saved in `m_i2p_sam_session`, so make sure that
     * the lifetime of `interruptNet` is not shorter than
     * the lifetime of `m_i2p_sam_session`.
     */
    CThreadInterrupt interruptNet;

    /**
     * I2P SAM session.
     * Used to accept incoming and make outgoing I2P connections.
     */
    std::unique_ptr<i2p::sam::Session> m_i2p_sam_session;

    std::thread threadDNSAddressSeed;
    std::thread threadSocketHandler;
    std::thread threadOpenAddedConnections;
    std::thread threadOpenConnections;
    std::thread threadMessageHandler;
    std::thread threadI2PAcceptIncoming;

    /** flag for deciding to connect to an extra outbound peer,
     *  in excess of m_max_outbound_full_relay
     *  This takes the place of a feeler connection */
    std::atomic_bool m_try_another_outbound_peer;

    /** flag for initiating extra block-relay-only peer connections.
     *  this should only be enabled after initial chain sync has occurred,
     *  as these connections are intended to be short-lived and low-bandwidth.
     */
    std::atomic_bool m_start_extra_block_relay_peers{false};

    /**
     * A vector of -bind=<address>:<port>=onion arguments each of which is
     * an address and port that are designated for incoming Tor connections.
     */
    std::vector<CService> m_onion_binds;

    /**
     * RAII helper to atomically create a copy of `m_nodes` and add a reference
     * to each of the nodes. The nodes are released when this object is destroyed.
     */
    class NodesSnapshot
    {
    public:
        explicit NodesSnapshot(const CConnman& connman, bool shuffle)
        {
            {
                LOCK(connman.m_nodes_mutex);
                m_nodes_copy = connman.m_nodes;
                for (auto& node : m_nodes_copy) {
                    node->AddRef();
                }
            }
            if (shuffle) {
                Shuffle(m_nodes_copy.begin(), m_nodes_copy.end(), FastRandomContext{});
            }
        }

        ~NodesSnapshot()
        {
            for (auto& node : m_nodes_copy) {
                node->Release();
            }
        }

        const std::vector<CNode*>& Nodes() const
        {
            return m_nodes_copy;
        }

    private:
        std::vector<CNode*> m_nodes_copy;
    };

    friend struct CConnmanTest;
    friend struct ConnmanTestMsg;
};

/** Dump binary message to file, with timestamp */
void CaptureMessageToFile(const CAddress& addr,
                          const std::string& msg_type,
                          Span<const unsigned char> data,
                          bool is_incoming);

/** Defaults to `CaptureMessageToFile()`, but can be overridden by unit tests. */
extern std::function<void(const CAddress& addr,
                          const std::string& msg_type,
                          Span<const unsigned char> data,
                          bool is_incoming)>
    CaptureMessage;

struct NodeEvictionCandidate
{
    NodeId id;
    std::chrono::seconds m_connected;
    std::chrono::microseconds m_min_ping_time;
    std::chrono::seconds m_last_block_time;
    std::chrono::seconds m_last_tx_time;
    bool fRelevantServices;
    bool fRelayTxes;
    bool fBloomFilter;
    uint64_t nKeyedNetGroup;
    bool prefer_evict;
    bool m_is_local;
    Network m_network;
};

/**
 * Select an inbound peer to evict after filtering out (protecting) peers having
 * distinct, difficult-to-forge characteristics. The protection logic picks out
 * fixed numbers of desirable peers per various criteria, followed by (mostly)
 * ratios of desirable or disadvantaged peers. If any eviction candidates
 * remain, the selection logic chooses a peer to evict.
 */
[[nodiscard]] std::optional<NodeId> SelectNodeToEvict(std::vector<NodeEvictionCandidate>&& vEvictionCandidates);

/** Protect desirable or disadvantaged inbound peers from eviction by ratio.
 *
 * This function protects half of the peers which have been connected the
 * longest, to replicate the non-eviction implicit behavior and preclude attacks
 * that start later.
 *
 * Half of these protected spots (1/4 of the total) are reserved for the
 * following categories of peers, sorted by longest uptime, even if they're not
 * longest uptime overall:
 *
 * - onion peers connected via our tor control service
 *
 * - localhost peers, as manually configured hidden services not using
 *   `-bind=addr[:port]=onion` will not be detected as inbound onion connections
 *
 * - I2P peers
 *
 * - CJDNS peers
 *
 * This helps protect these privacy network peers, which tend to be otherwise
 * disadvantaged under our eviction criteria for their higher min ping times
 * relative to IPv4/IPv6 peers, and favorise the diversity of peer connections.
 */
void ProtectEvictionCandidatesByRatio(std::vector<NodeEvictionCandidate>& vEvictionCandidates);

#endif // BITCOIN_NET_H
