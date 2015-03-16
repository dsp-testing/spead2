/**
 * @file
 */

#ifndef SPEAD_RECV_PACKET
#define SPEAD_RECV_PACKET

#include <cstddef>
#include <cstdint>
#include "common_defines.h"

namespace spead
{
namespace recv
{

/**
 * Unpacked packet header, with pointers to the original data.
 */
struct packet_header
{
    /// Number of bits in addresses/immediates (from SPEAD flavour)
    int heap_address_bits;
    /// Number of item pointers in the packet
    int n_items;
    /**
     * @name Key fields extracted from items in the packet
     * @{
     * The true values are always non-negative, and -1 is used to indicate
     * that the packet did not contain the item.
     */
    s_item_pointer_t heap_cnt;
    s_item_pointer_t heap_length;
    s_item_pointer_t payload_offset;
    s_item_pointer_t payload_length;
    /** @} */
    /// The item pointers in the packet, in big endian
    const item_pointer_t *pointers;
    /// Start of the packet payload
    const uint8_t *payload;
};

/**
 * Split out the header fields for the packet.
 *
 * @param[out] out     Packet header with pointers to data (undefined on failure)
 * @param[in]  raw     Start of packet
 * @param      max_size Size of data pointed to by @a raw
 * @returns Actual packet size on success, or 0 on failure (due to malformed or
 * truncated packet).
 *
 * @pre @a raw is 8-byte aligned and @a raw + 8 is aligned to @ref item_pointer_t
 */
std::size_t decode_packet(packet_header &out, const uint8_t *raw, std::size_t max_size);

} // namespace recv
} // namespace spead

#endif // SPEAD_RECV_PACKET
