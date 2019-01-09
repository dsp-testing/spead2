/* Copyright 2016-2019 SKA South Africa
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 */

#include <spead2/common_features.h>
#if SPEAD2_USE_IBV
#include <cerrno>
#include <cstring>
#include <cassert>
#include <memory>
#include <algorithm>
#include <system_error>
#include <boost/asio.hpp>
#include <spead2/common_logging.h>
#include <spead2/common_ibv.h>
#include <spead2/common_semaphore.h>
#include <spead2/common_endian.h>
#include <spead2/common_raw_packet.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

namespace spead2
{

namespace detail
{

#if SPEAD2_USE_IBV_MPRQ
ibv_intf_deleter::ibv_intf_deleter(struct ibv_context *context) noexcept : context(context) {}

void ibv_intf_deleter::operator()(void *intf)
{
    assert(context);
    struct ibv_exp_release_intf_params params;
    std::memset(&params, 0, sizeof(params));
    ibv_exp_release_intf(context, intf, &params);
}
#endif

} // namespace detail

rdma_event_channel_t::rdma_event_channel_t()
{
    errno = 0;
    rdma_event_channel *event_channel = rdma_create_event_channel();
    if (!event_channel)
        throw_errno("rdma_create_event_channel failed");
    reset(event_channel);
}

rdma_cm_id_t::rdma_cm_id_t(const rdma_event_channel_t &event_channel, void *context, rdma_port_space ps)
{
    rdma_cm_id *cm_id = nullptr;
    errno = 0;
    int status = rdma_create_id(event_channel.get(), &cm_id, context, ps);
    if (status < 0)
        throw_errno("rdma_create_id failed");
    reset(cm_id);
}

void rdma_cm_id_t::bind_addr(const boost::asio::ip::address &addr)
{
    assert(get());
    boost::asio::ip::udp::endpoint endpoint(addr, 0);
    errno = 0;
    int status = rdma_bind_addr(get(), endpoint.data());
    if (status < 0)
        throw_errno("rdma_bind_addr failed");
    if (get()->verbs == nullptr)
        throw_errno("rdma_bind_addr did not bind to an RDMA device", ENODEV);
}

ibv_device_attr rdma_cm_id_t::query_device() const
{
    assert(get());
    ibv_device_attr attr;
    int status = ibv_query_device(get()->verbs, &attr);
    if (status != 0)
        throw_errno("ibv_query_device failed", status);
    return attr;
}

ibv_context_t::ibv_context_t(struct ibv_device *device)
{
    ibv_context *ctx = ibv_open_device(device);
    if (!ctx)
        throw_errno("ibv_open_device failed");
    reset(ctx);
}

ibv_context_t::ibv_context_t(const boost::asio::ip::address &addr)
{
    /* Use rdma_cm_id_t to get an existing device context, then
     * query it for its GUID and find the corresponding device.
     */
    rdma_event_channel_t event_channel;
    rdma_cm_id_t cm_id(event_channel, nullptr, RDMA_PS_UDP);
    cm_id.bind_addr(addr);
    ibv_device_attr attr = cm_id.query_device();

    struct ibv_device **devices;
    devices = ibv_get_device_list(nullptr);
    if (devices == nullptr)
        throw_errno("ibv_get_device_list failed");

    ibv_device *device = nullptr;
    for (ibv_device **d = devices; *d != nullptr; d++)
        if (ibv_get_device_guid(*d) == attr.node_guid)
        {
            device = *d;
            break;
        }
    if (device == nullptr)
    {
        ibv_free_device_list(devices);
        throw_errno("no matching device found", ENOENT);
    }

    ibv_context *ctx = ibv_open_device(device);
    if (!ctx)
    {
        ibv_free_device_list(devices);
        throw_errno("ibv_open_device failed");
    }
    reset(ctx);
    ibv_free_device_list(devices);
}

ibv_comp_channel_t::ibv_comp_channel_t(const rdma_cm_id_t &cm_id)
{
    errno = 0;
    ibv_comp_channel *comp_channel = ibv_create_comp_channel(cm_id->verbs);
    if (!comp_channel)
        throw_errno("ibv_create_comp_channel failed");
    reset(comp_channel);
}

boost::asio::posix::stream_descriptor ibv_comp_channel_t::wrap(
    boost::asio::io_service &io_service) const
{
    assert(get());
    return wrap_fd(io_service, get()->fd);
}

void ibv_comp_channel_t::get_event(ibv_cq **cq, void **context)
{
    assert(get());
    errno = 0;
    int status = ibv_get_cq_event(get(), cq, context);
    if (status < 0)
        throw_errno("ibv_get_cq_event failed");
}

ibv_cq_t::ibv_cq_t(
    const rdma_cm_id_t &cm_id, int cqe, void *context,
    const ibv_comp_channel_t &comp_channel, int comp_vector)
{
    errno = 0;
    ibv_cq *cq = ibv_create_cq(cm_id->verbs, cqe, context, comp_channel.get(), comp_vector);
    if (!cq)
        throw_errno("ibv_create_cq failed");
    reset(cq);
}

ibv_cq_t::ibv_cq_t(const rdma_cm_id_t &cm_id, int cqe, void *context)
{
    errno = 0;
    ibv_cq *cq = ibv_create_cq(cm_id->verbs, cqe, context, nullptr, 0);
    if (!cq)
        throw_errno("ibv_create_cq failed");
    reset(cq);
}

#if SPEAD2_USE_IBV_EXP
ibv_cq_t::ibv_cq_t(
    const rdma_cm_id_t &cm_id, int cqe, void *context,
    const ibv_comp_channel_t &comp_channel, int comp_vector,
    ibv_exp_cq_init_attr *attr)
{
    errno = 0;
    ibv_cq *cq = ibv_exp_create_cq(cm_id->verbs, cqe, context, comp_channel.get(), comp_vector, attr);
    if (!cq)
        throw_errno("ibv_create_cq failed");
    reset(cq);
}

ibv_cq_t::ibv_cq_t(
    const rdma_cm_id_t &cm_id, int cqe, void *context,
    ibv_exp_cq_init_attr *attr)
{
    errno = 0;
    ibv_cq *cq = ibv_exp_create_cq(cm_id->verbs, cqe, context, nullptr, 0, attr);
    if (!cq)
        throw_errno("ibv_create_cq failed");
    reset(cq);
}
#endif // SPEAD2_USE_IBV_EXP

void ibv_cq_t::req_notify(bool solicited_only)
{
    assert(get());
    int status = ibv_req_notify_cq(get(), int(solicited_only));
    if (status != 0)
        throw_errno("ibv_req_notify_cq failed", status);
}

int ibv_cq_t::poll(int num_entries, ibv_wc *wc)
{
    assert(get());
    int received = ibv_poll_cq(get(), num_entries, wc);
    if (received < 0)
        throw_errno("ibv_poll_cq failed");
    return received;
}

#if SPEAD2_USE_IBV_EXP
int ibv_cq_t::poll(int num_entries, ibv_exp_wc *wc)
{
    assert(get());
    int received = ibv_exp_poll_cq(get(), num_entries, wc, sizeof(wc[0]));
    if (received < 0)
        throw_errno("ibv_exp_poll_cq failed");
    return received;
}
#endif

void ibv_cq_t::ack_events(unsigned int nevents)
{
    assert(get());
    ibv_ack_cq_events(get(), nevents);
}

ibv_pd_t::ibv_pd_t(const rdma_cm_id_t &cm_id)
{
    errno = 0;
    ibv_pd *pd = ibv_alloc_pd(cm_id->verbs);
    if (!pd)
        throw_errno("ibv_alloc_pd failed");
    reset(pd);
}

ibv_qp_t::ibv_qp_t(const ibv_pd_t &pd, ibv_qp_init_attr *init_attr)
{
    errno = 0;
    ibv_qp *qp = ibv_create_qp(pd.get(), init_attr);
    if (!qp)
        throw_errno("ibv_create_qp failed");
    reset(qp);
}

#if SPEAD2_USE_IBV_MPRQ
ibv_qp_t::ibv_qp_t(const rdma_cm_id_t &cm_id, ibv_exp_qp_init_attr *init_attr)
{
    errno = 0;
    ibv_qp *qp = ibv_exp_create_qp(cm_id->verbs, init_attr);
    if (!qp)
        throw_errno("ibv_exp_create_qp failed");
    reset(qp);
}
#endif

ibv_mr_t::ibv_mr_t(const ibv_pd_t &pd, void *addr, std::size_t length, int access)
{
    errno = 0;
    ibv_mr * mr = ibv_reg_mr(pd.get(), addr, length, IBV_ACCESS_LOCAL_WRITE);
    if (!mr)
        throw_errno("ibv_reg_mr failed");
    reset(mr);
}

void ibv_qp_t::modify(ibv_qp_attr *attr, int attr_mask)
{
    assert(get());
    int status = ibv_modify_qp(get(), attr, attr_mask);
    if (status != 0)
        throw_errno("ibv_modify_qp failed", status);
}

void ibv_qp_t::modify(ibv_qp_state qp_state)
{
    ibv_qp_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.qp_state = qp_state;
    modify(&attr, IBV_QP_STATE);
}

void ibv_qp_t::modify(ibv_qp_state qp_state, int port_num)
{
    ibv_qp_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.qp_state = qp_state;
    attr.port_num = port_num;
    modify(&attr, IBV_QP_STATE | IBV_QP_PORT);
}

void ibv_qp_t::post_recv(ibv_recv_wr *wr)
{
    assert(get());
    ibv_recv_wr *bad_wr;
    int status = ibv_post_recv(get(), wr, &bad_wr);
    if (status != 0)
        throw_errno("ibv_post_recv failed", status);
}

void ibv_qp_t::post_send(ibv_send_wr *wr)
{
    assert(get());
    ibv_send_wr *bad_wr;
    int status = ibv_post_send(get(), wr, &bad_wr);
    if (status != 0)
        throw_errno("ibv_post_send failed", status);
}

ibv_flow_t::ibv_flow_t(const ibv_qp_t &qp, ibv_flow_attr *flow_attr)
{
    errno = 0;
    ibv_flow *flow = ibv_create_flow(qp.get(), flow_attr);
    if (!flow)
        throw_errno("ibv_create_flow failed");
    reset(flow);
}

/**
 * Check if @a b is the next IP address after @a a.
 *
 * Behaviour is undefined if the addresses are not multicast addresses (in particular,
 * it does not try to cater for wraparound from 255.255.255.255 to 0.0.0.0).
 */
static bool sequential(const boost::asio::ip::address_v4 &a,
                       const boost::asio::ip::address_v4 &b)
{
    std::uint32_t a_raw = a.to_ulong();
    std::uint32_t b_raw = b.to_ulong();
    return b_raw == a_raw + 1;
}

/**
 * Check if @a a and @a b have the same port and sequential IPv4 addresses.
 *
 * Behaviour is undefined if the addresses are not IPv4 multicast addresses.
 */
static bool sequential(const boost::asio::ip::udp::endpoint &a,
                       const boost::asio::ip::udp::endpoint &b)
{
    return a.port() == b.port() && sequential(a.address().to_v4(), b.address().to_v4());
}

ibv_flow_t create_flow(
    const ibv_qp_t &qp, const boost::asio::ip::udp::endpoint &endpoint,
    int port_num, std::uint32_t mask)
{
    struct
    {
        ibv_flow_attr attr;
        ibv_flow_spec_eth eth;
        ibv_flow_spec_ipv4 ip;
        ibv_flow_spec_tcp_udp udp;
    } __attribute__((packed)) flow_rule;
    std::memset(&flow_rule, 0, sizeof(flow_rule));

    flow_rule.attr.type = IBV_FLOW_ATTR_NORMAL;
    flow_rule.attr.priority = 0;
    flow_rule.attr.size = sizeof(flow_rule);
    flow_rule.attr.num_of_specs = 3;
    flow_rule.attr.port = port_num;

    /* At least the ConnectX-3 cards seem to require an Ethernet match. We
     * thus have to construct the Ethernet multicast address corresponding to
     * the IP multicast address from RFC 7042.
     */
    flow_rule.eth.type = IBV_FLOW_SPEC_ETH;
    flow_rule.eth.size = sizeof(flow_rule.eth);
    mac_address dst_mac = multicast_mac(endpoint.address());
    std::memcpy(&flow_rule.eth.val.dst_mac, &dst_mac, sizeof(dst_mac));
    /* Set mask. Multicast MAC addresses only encode the bottom 23 bits. */
    std::uint32_t mac_mask = mask | 0xFF800000;
    std::memset(&flow_rule.eth.mask.dst_mac, 0xFF, sizeof(flow_rule.eth.mask.dst_mac));
    for (int i = 0; i < 4; i++)
        flow_rule.eth.mask.dst_mac[5 - i] = (mac_mask >> (8 * i)) & 0xFF;

    flow_rule.ip.type = IBV_FLOW_SPEC_IPV4;
    flow_rule.ip.size = sizeof(flow_rule.ip);
    auto bytes = endpoint.address().to_v4().to_bytes(); // big-endian address
    std::memcpy(&flow_rule.ip.val.dst_ip, &bytes, sizeof(bytes));
    flow_rule.ip.mask.dst_ip = htobe(mask);

    flow_rule.udp.type = IBV_FLOW_SPEC_UDP;
    flow_rule.udp.size = sizeof(flow_rule.udp);
    flow_rule.udp.val.dst_port = htobe16(endpoint.port());
    flow_rule.udp.mask.dst_port = 0xFFFF;

    return ibv_flow_t(qp, &flow_rule.attr);
}

std::vector<ibv_flow_t> create_flows(
    const ibv_qp_t &qp,
    const std::vector<boost::asio::ip::udp::endpoint> &endpoints,
    int port_num)
{
    /* Compare endpoints by port first (unlike the builtin comparison), so that
     * a sequence of addresses on the same port sort together.
     */
    auto cmp_endpoints = [](const boost::asio::ip::udp::endpoint &e1,
                            const boost::asio::ip::udp::endpoint &e2) -> bool
    {
        if (e1.port() != e2.port())
            return e1.port() < e2.port();
        else
            return e1.address() < e2.address();
    };

    // Get a sorted list of endpoints, using the given ones if already sorted
    std::vector<boost::asio::ip::udp::endpoint> endpoints_copy;
    bool is_sorted = std::is_sorted(endpoints.begin(), endpoints.end(), cmp_endpoints);
    if (!is_sorted)
    {
        endpoints_copy = endpoints;
        std::sort(endpoints_copy.begin(), endpoints_copy.end(), cmp_endpoints);
    }
    auto &ep = is_sorted ? endpoints : endpoints_copy;

    std::vector<ibv_flow_t> flows;
    auto pos = ep.begin();
    while (pos != ep.end())
    {
        // Find range of contiguous addresses with the same port
        auto cur = pos;
        auto tail = std::next(pos);
        while (tail != ep.end() && sequential(*cur, *tail))
        {
            cur = tail;
            ++tail;
        }
        // Break up this range into aligned power-of-two groups.
        std::size_t n = tail - pos;
        while (n > 0)
        {
            std::uint32_t pos_raw = pos->address().to_v4().to_ulong();
            std::uint32_t mask = 0xFFFFFFFF;
            while (mask != 0 && (pos_raw & ~(mask << 1)) == 0 && ~(mask << 1) < n)
                mask <<= 1;
            try
            {
                flows.push_back(create_flow(qp, *pos, port_num, mask));
                pos += ~mask + 1;
                n -= ~mask + 1;
            }
            catch (std::system_error)
            {
                /* Driver doesn't support non-trivial masks. Fall back to
                 * adding a separate flow rule per endpoint.
                 */
                while (pos != ep.end())
                {
                    flows.push_back(create_flow(qp, *pos, port_num));
                    ++pos;
                }
                n = 0;
            }
        }
        pos = tail;
    }
    return flows;
}

#if SPEAD2_USE_IBV_MPRQ

const char *ibv_exp_query_intf_error_category::name() const noexcept
{
    return "ibv_exp_query_intf";
}

std::string ibv_exp_query_intf_error_category::message(int condition) const
{
    switch (condition)
    {
    case IBV_EXP_INTF_STAT_OK:
        return "OK";
    case IBV_EXP_INTF_STAT_VENDOR_NOT_SUPPORTED:
        return "The provided 'vendor_guid' is not supported";
    case IBV_EXP_INTF_STAT_INTF_NOT_SUPPORTED:
        return "The provided 'intf' is not supported";
    case IBV_EXP_INTF_STAT_VERSION_NOT_SUPPORTED:
        return "The provided 'intf_version' is not supported";
    case IBV_EXP_INTF_STAT_INVAL_PARARM:
        return "General invalid parameter";
    case IBV_EXP_INTF_STAT_INVAL_OBJ_STATE:
        return "QP is not in INIT, RTR or RTS state";
    case IBV_EXP_INTF_STAT_INVAL_OBJ:
        return "Mismatch between the provided 'obj'(CQ/QP/WQ) and requested 'intf'";
    case IBV_EXP_INTF_STAT_FLAGS_NOT_SUPPORTED:
        return "The provided set of 'flags' is not supported";
    case IBV_EXP_INTF_STAT_FAMILY_FLAGS_NOT_SUPPORTED:
        return "The provided set of 'family_flags' is not supported";
    default:
        return "Unknown error";
    }
}

std::error_condition ibv_exp_query_intf_error_category::default_error_condition(int condition) const noexcept
{
    switch (condition)
    {
    case IBV_EXP_INTF_STAT_VENDOR_NOT_SUPPORTED:
    case IBV_EXP_INTF_STAT_INTF_NOT_SUPPORTED:
    case IBV_EXP_INTF_STAT_VERSION_NOT_SUPPORTED:
    case IBV_EXP_INTF_STAT_FLAGS_NOT_SUPPORTED:
    case IBV_EXP_INTF_STAT_FAMILY_FLAGS_NOT_SUPPORTED:
        return std::errc::not_supported;
    case IBV_EXP_INTF_STAT_INVAL_PARARM:
    case IBV_EXP_INTF_STAT_INVAL_OBJ_STATE:
    case IBV_EXP_INTF_STAT_INVAL_OBJ:
        return std::errc::invalid_argument;
    default:
        return std::error_condition(condition, *this);
    }
}

std::error_category &ibv_exp_query_intf_category()
{
    static ibv_exp_query_intf_error_category category;
    return category;
}

static void *query_intf(const rdma_cm_id_t &cm_id, ibv_exp_query_intf_params *params)
{
    ibv_exp_query_intf_status status;
    void *intf = ibv_exp_query_intf(cm_id->verbs, params, &status);
    if (status != IBV_EXP_INTF_STAT_OK)
    {
        std::error_code code(status, ibv_exp_query_intf_category());
        throw std::system_error(code, "ibv_exp_query_intf failed");
    }
    return intf;
}

ibv_exp_cq_family_v1_t::ibv_exp_cq_family_v1_t(const rdma_cm_id_t &cm_id, const ibv_cq_t &cq)
    : std::unique_ptr<ibv_exp_cq_family_v1, detail::ibv_intf_deleter>(
        nullptr, detail::ibv_intf_deleter(cm_id->verbs))
{
    ibv_exp_query_intf_params params;
    std::memset(&params, 0, sizeof(params));
    params.intf_scope = IBV_EXP_INTF_GLOBAL;
    params.intf = IBV_EXP_INTF_CQ;
    params.intf_version = 1;
    params.obj = cq.get();
    void *intf = query_intf(cm_id, &params);
    reset(static_cast<ibv_exp_cq_family_v1 *>(intf));
}

ibv_exp_wq_t::ibv_exp_wq_t(const rdma_cm_id_t &cm_id, ibv_exp_wq_init_attr *attr)
{
    ibv_exp_wq *wq = ibv_exp_create_wq(cm_id->verbs, attr);
    if (!wq)
        throw_errno("ibv_exp_create_wq failed");
    reset(wq);
}

void ibv_exp_wq_t::modify(ibv_exp_wq_state state)
{
    ibv_exp_wq_attr wq_attr;
    memset(&wq_attr, 0, sizeof(wq_attr));
    wq_attr.wq_state = IBV_EXP_WQS_RDY;
    wq_attr.attr_mask = IBV_EXP_WQ_ATTR_STATE;
    int status = ibv_exp_modify_wq(get(), &wq_attr);
    if (status != 0)
        throw_errno("ibv_exp_modify_wq failed", status);
}

ibv_exp_wq_family_t::ibv_exp_wq_family_t(const rdma_cm_id_t &cm_id, const ibv_exp_wq_t &wq)
    : std::unique_ptr<ibv_exp_wq_family, detail::ibv_intf_deleter>(
        nullptr, detail::ibv_intf_deleter(cm_id->verbs))
{
    ibv_exp_query_intf_params params;
    std::memset(&params, 0, sizeof(params));
    params.intf_scope = IBV_EXP_INTF_GLOBAL;
    params.intf = IBV_EXP_INTF_WQ;
    params.obj = wq.get();
    void *intf = query_intf(cm_id, &params);
    reset(static_cast<ibv_exp_wq_family *>(intf));
}

ibv_exp_rwq_ind_table_t::ibv_exp_rwq_ind_table_t(const rdma_cm_id_t &cm_id, ibv_exp_rwq_ind_table_init_attr *attr)
{
    ibv_exp_rwq_ind_table *table = ibv_exp_create_rwq_ind_table(cm_id->verbs, attr);
    if (!table)
        throw_errno("ibv_exp_create_rwq_ind_table failed");
    reset(table);
}

ibv_exp_rwq_ind_table_t create_rwq_ind_table(
    const rdma_cm_id_t &cm_id, const ibv_pd_t &pd, const ibv_exp_wq_t &wq)
{
    ibv_exp_rwq_ind_table_init_attr attr;
    ibv_exp_wq *tbl[1] = {wq.get()};
    std::memset(&attr, 0, sizeof(attr));
    attr.pd = pd.get();
    attr.log_ind_tbl_size = 0;
    attr.ind_tbl = tbl;
    return ibv_exp_rwq_ind_table_t(cm_id, &attr);
}

#endif // SPEAD2_USE_IBV_MPRQ

} // namespace spead

#endif // SPEAD2_USE_IBV
