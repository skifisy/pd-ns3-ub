// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_LINK_H
#define UB_LINK_H

#include <list>
#include "ns3/channel.h"
#include "ns3/point-to-point-channel.h"
#include "ns3/ptr.h"
#include "ns3/nstime.h"
#include "ns3/data-rate.h"
#include "ns3/traced-callback.h"
#include "ns3/simulator.h"
#include "ns3/packet.h"
#include "ns3/ub-port.h"

namespace ns3 {

class UbPort;
class Packet;

/**
 * @ingroup point-to-point
 * @brief Simple Point To Point link.
 *
 * This class represents a very simple ub link.  Think full
 * duplex RS-232 or RS-422 with null modem and no handshaking.  There is no
 * multi-drop capability on this link -- there can be a maximum of two
 * ub devices connected.
 *
 * There are two "wires" in the link.  The first device connected gets the
 * [0] wire to transmit on.  The second device gets the [1] wire.  There is a
 * state (IDLE, TRANSMITTING) associated with each wire.
 */
class UbLink : public PointToPointChannel {
public:
    static TypeId GetTypeId(void);

    // Bring base-class overloads of TransmitStart into scope to avoid hiding warnings
    using PointToPointChannel::TransmitStart;

    /**
     * @brief Create a UbLink
     *
     * By default, you get a link that
     * has zero transmission delay.
     */
    UbLink();

    /**
     * @brief Attach a given netdevice to this link
     * @param device pointer to the ubdevice to attach to the link
     */
    virtual void Attach(Ptr<UbPort> device);
    
    /**
    * @brief Transmit a packet over this link (UbPort version)
    * @param p Packet to transmit
    * @param src Source UbPort
    * @param txTime Transmit time to apply
    * @returns true if successful (currently always true)
    */
    virtual bool TransmitStart(Ptr<Packet> p, Ptr<UbPort> src, Time txTime);

    virtual bool IsRemote(void) const;

    /**
     * @brief Get number of devices on this link
     * @returns number of devices on this link
     */
    virtual size_t GetNDevices(void) const;

    /**
     * @brief Get UbPort corresponding to index i on this link
     * @param i Index number of the device requested
     * @returns Ptr to UbPort requested
     */
    Ptr<UbPort> GetUbPort(uint32_t i) const;

    /**
     * @brief Get NetDevice corresponding to index i on this link
     * @param i Index number of the device requested
     * @returns Ptr to NetDevice requested
     */
    virtual Ptr<NetDevice> GetDevice(std::size_t i) const;

    /**
     * @brief Get the delay associated with this channel
     * @returns Time delay
     */
    Time GetDelay(void) const;

    Ptr<UbPort> GetDestination(Ptr<UbPort> src) const;

protected:
    /**
     * @brief Check to make sure the link is initialized
     * @returns true if initialized, asserts otherwise
     */
    bool IsInitialized(void) const;

    /**
     * @brief Get the net-device source
     * @param i the link requested
     * @returns Ptr to UbPort source for the
     * specified link
     */
    Ptr<UbPort> GetSource(uint32_t i) const;

    /**
     * @brief Get the net-device destination
     * @param i the link requested
     * @returns Ptr to UbPort destination for
     * the specified link
     */
    Ptr<UbPort> GetDestination(uint32_t i) const;

private:
    // Each link has exactly two net devices
    static const int nDevices = 2;

    Time m_delay;
    int32_t m_nDevices;

    /** @brief Wire states
     *
     */
    enum class WireState {
        /** Initializing state */
        INITIALIZING,
        /** Idle state (no transmission from NetDevice) */
        IDLE,
        /** Transmitting state (data being transmitted from NetDevice. */
        TRANSMITTING,
        /** Propagating state (data is being propagated in the link. */
        PROPAGATING
    };

    class Link {
    public:
        Link() : m_state(WireState::INITIALIZING), m_src(0), m_dst(0) {}
        WireState m_state;
        Ptr<UbPort> m_src;
        Ptr<UbPort> m_dst;
    };

    Link m_link[nDevices];
};

} // namespace ns3

#endif /* UB_LINK_H */
