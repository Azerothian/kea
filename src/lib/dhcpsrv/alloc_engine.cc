// Copyright (C) 2012-2015 Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include <dhcpsrv/alloc_engine.h>
#include <dhcpsrv/dhcpsrv_log.h>
#include <dhcpsrv/host_mgr.h>
#include <dhcpsrv/host.h>
#include <dhcpsrv/lease_mgr_factory.h>
#include <dhcp/dhcp6.h>

#include <hooks/server_hooks.h>
#include <hooks/hooks_manager.h>

#include <cstring>
#include <limits>
#include <vector>
#include <stdint.h>
#include <string.h>

using namespace isc::asiolink;
using namespace isc::dhcp;
using namespace isc::hooks;

namespace {

/// Structure that holds registered hook indexes
struct AllocEngineHooks {
    int hook_index_lease4_select_; ///< index for "lease4_receive" hook point
    int hook_index_lease4_renew_;  ///< index for "lease4_renew" hook point
    int hook_index_lease6_select_; ///< index for "lease6_receive" hook point
    int hook_index_lease6_renew_;  ///< index for "lease6_renew" hook point
    int hook_index_lease6_rebind_; ///< index for "lease6_rebind" hook point

    /// Constructor that registers hook points for AllocationEngine
    AllocEngineHooks() {
        hook_index_lease4_select_ = HooksManager::registerHook("lease4_select");
        hook_index_lease4_renew_  = HooksManager::registerHook("lease4_renew");
        hook_index_lease6_select_ = HooksManager::registerHook("lease6_select");
        hook_index_lease6_renew_   = HooksManager::registerHook("lease6_renew");
        hook_index_lease6_rebind_   = HooksManager::registerHook("lease6_rebind");
    }
};

// Declare a Hooks object. As this is outside any function or method, it
// will be instantiated (and the constructor run) when the module is loaded.
// As a result, the hook indexes will be defined before any method in this
// module is called.
AllocEngineHooks Hooks;

}; // anonymous namespace

namespace isc {
namespace dhcp {

AllocEngine::IterativeAllocator::IterativeAllocator(Lease::Type lease_type)
    :Allocator(lease_type) {
}

isc::asiolink::IOAddress
AllocEngine::IterativeAllocator::increasePrefix(const isc::asiolink::IOAddress& prefix,
                                                const uint8_t prefix_len) {
    if (!prefix.isV6()) {
        isc_throw(BadValue, "Prefix operations are for IPv6 only (attempted to "
                  "increase prefix " << prefix << ")");
    }

    // Get a buffer holding an address.
    const std::vector<uint8_t>& vec = prefix.toBytes();

    if (prefix_len < 1 || prefix_len > 128) {
        isc_throw(BadValue, "Cannot increase prefix: invalid prefix length: "
                  << prefix_len);
    }

    // Brief explanation what happens here:
    // http://www.youtube.com/watch?v=NFQCYpIHLNQ

    uint8_t n_bytes = (prefix_len - 1)/8;
    uint8_t n_bits = 8 - (prefix_len - n_bytes*8);
    uint8_t mask = 1 << n_bits;

    // Longer explanation: n_bytes specifies number of full bytes that are
    // in-prefix. They can also be used as an offset for the first byte that
    // is not in prefix. n_bits specifies number of bits on the last byte that
    // is (often partially) in prefix. For example for a /125 prefix, the values
    // are 15 and 3, respectively. Mask is a bitmask that has the least
    // significant bit from the prefix set.

    uint8_t packed[V6ADDRESS_LEN];

    // Copy the address. It must be V6, but we already checked that.
    std::memcpy(packed, &vec[0], V6ADDRESS_LEN);

    // Can we safely increase only the last byte in prefix without overflow?
    if (packed[n_bytes] + uint16_t(mask) < 256u) {
        packed[n_bytes] += mask;
        return (IOAddress::fromBytes(AF_INET6, packed));
    }

    // Overflow (done on uint8_t, but the sum is greater than 255)
    packed[n_bytes] += mask;

    // Deal with the overflow. Start increasing the least significant byte
    for (int i = n_bytes - 1; i >= 0; --i) {
        ++packed[i];
        // If we haven't overflowed (0xff->0x0) the next byte, then we are done
        if (packed[i] != 0) {
            break;
        }
    }

    return (IOAddress::fromBytes(AF_INET6, packed));
}


isc::asiolink::IOAddress
AllocEngine::IterativeAllocator::pickAddress(const SubnetPtr& subnet,
                                             const DuidPtr&,
                                             const IOAddress&) {

    // Is this prefix allocation?
    bool prefix = pool_type_ == Lease::TYPE_PD;

    // Let's get the last allocated address. It is usually set correctly,
    // but there are times when it won't be (like after removing a pool or
    // perhaps restarting the server).
    IOAddress last = subnet->getLastAllocated(pool_type_);

    const PoolCollection& pools = subnet->getPools(pool_type_);

    if (pools.empty()) {
        isc_throw(AllocFailed, "No pools defined in selected subnet");
    }

    // first we need to find a pool the last address belongs to.
    PoolCollection::const_iterator it;
    for (it = pools.begin(); it != pools.end(); ++it) {
        if ((*it)->inRange(last)) {
            break;
        }
    }

    // last one was bogus for one of several reasons:
    // - we just booted up and that's the first address we're allocating
    // - a subnet was removed or other reconfiguration just completed
    // - perhaps allocation algorithm was changed
    if (it == pools.end()) {
        // ok to access first element directly. We checked that pools is non-empty
        IOAddress next = pools[0]->getFirstAddress();
        subnet->setLastAllocated(pool_type_, next);
        return (next);
    }

    // Ok, we have a pool that the last address belonged to, let's use it.

    IOAddress next("::");
    if (!prefix) {
        next = IOAddress::increase(last); // basically addr++
    } else {
        Pool6Ptr pool6 = boost::dynamic_pointer_cast<Pool6>(*it);
        if (!pool6) {
            // Something is gravely wrong here
            isc_throw(Unexpected, "Wrong type of pool: " << (*it)->toText()
                      << " is not Pool6");
        }
        // Get the next prefix
        next = increasePrefix(last, pool6->getLength());
    }
    if ((*it)->inRange(next)) {
        // the next one is in the pool as well, so we haven't hit pool boundary yet
        subnet->setLastAllocated(pool_type_, next);
        return (next);
    }

    // We hit pool boundary, let's try to jump to the next pool and try again
    ++it;
    if (it == pools.end()) {
        // Really out of luck today. That was the last pool. Let's rewind
        // to the beginning.
        next = pools[0]->getFirstAddress();
        subnet->setLastAllocated(pool_type_, next);
        return (next);
    }

    // there is a next pool, let's try first address from it
    next = (*it)->getFirstAddress();
    subnet->setLastAllocated(pool_type_, next);
    return (next);
}

AllocEngine::HashedAllocator::HashedAllocator(Lease::Type lease_type)
    :Allocator(lease_type) {
    isc_throw(NotImplemented, "Hashed allocator is not implemented");
}


isc::asiolink::IOAddress
AllocEngine::HashedAllocator::pickAddress(const SubnetPtr&,
                                          const DuidPtr&,
                                          const IOAddress&) {
    isc_throw(NotImplemented, "Hashed allocator is not implemented");
}

AllocEngine::RandomAllocator::RandomAllocator(Lease::Type lease_type)
    :Allocator(lease_type) {
    isc_throw(NotImplemented, "Random allocator is not implemented");
}


isc::asiolink::IOAddress
AllocEngine::RandomAllocator::pickAddress(const SubnetPtr&,
                                          const DuidPtr&,
                                          const IOAddress&) {
    isc_throw(NotImplemented, "Random allocator is not implemented");
}


AllocEngine::AllocEngine(AllocType engine_type, unsigned int attempts,
                         bool ipv6)
    :attempts_(attempts) {

    // Choose the basic (normal address) lease type
    Lease::Type basic_type = ipv6 ? Lease::TYPE_NA : Lease::TYPE_V4;

    // Initalize normal address allocators
    switch (engine_type) {
    case ALLOC_ITERATIVE:
        allocators_[basic_type] = AllocatorPtr(new IterativeAllocator(basic_type));
        break;
    case ALLOC_HASHED:
        allocators_[basic_type] = AllocatorPtr(new HashedAllocator(basic_type));
        break;
    case ALLOC_RANDOM:
        allocators_[basic_type] = AllocatorPtr(new RandomAllocator(basic_type));
        break;
    default:
        isc_throw(BadValue, "Invalid/unsupported allocation algorithm");
    }

    // If this is IPv6 allocation engine, initalize also temporary addrs
    // and prefixes
    if (ipv6) {
        switch (engine_type) {
        case ALLOC_ITERATIVE:
            allocators_[Lease::TYPE_TA] = AllocatorPtr(new IterativeAllocator(Lease::TYPE_TA));
            allocators_[Lease::TYPE_PD] = AllocatorPtr(new IterativeAllocator(Lease::TYPE_PD));
            break;
        case ALLOC_HASHED:
            allocators_[Lease::TYPE_TA] = AllocatorPtr(new HashedAllocator(Lease::TYPE_TA));
            allocators_[Lease::TYPE_PD] = AllocatorPtr(new HashedAllocator(Lease::TYPE_PD));
            break;
        case ALLOC_RANDOM:
            allocators_[Lease::TYPE_TA] = AllocatorPtr(new RandomAllocator(Lease::TYPE_TA));
            allocators_[Lease::TYPE_PD] = AllocatorPtr(new RandomAllocator(Lease::TYPE_PD));
            break;
        default:
            isc_throw(BadValue, "Invalid/unsupported allocation algorithm");
        }
    }

    // Register hook points
    hook_index_lease4_select_ = Hooks.hook_index_lease4_select_;
    hook_index_lease6_select_ = Hooks.hook_index_lease6_select_;
}

AllocEngine::AllocatorPtr AllocEngine::getAllocator(Lease::Type type) {
    std::map<Lease::Type, AllocatorPtr>::const_iterator alloc = allocators_.find(type);

    if (alloc == allocators_.end()) {
        isc_throw(BadValue, "No allocator initialized for pool type "
                  << Lease::typeToText(type));
    }
    return (alloc->second);
}

// ##########################################################################
// #    DHCPv6 lease allocation code starts here.
// ##########################################################################

AllocEngine::ClientContext6::ClientContext6()
    : subnet_(), duid_(), iaid_(0), type_(Lease::TYPE_NA), hwaddr_(),
      hints_(), fwd_dns_update_(false), rev_dns_update_(false), hostname_(""),
      callout_handle_(), fake_allocation_(false), old_leases_(), host_(),
      query_(), ia_rsp_(), allow_new_leases_in_renewals_(false) {
}

AllocEngine::ClientContext6::ClientContext6(const Subnet6Ptr& subnet, const DuidPtr& duid,
                                            const uint32_t iaid,
                                            const isc::asiolink::IOAddress& hint,
                                            const Lease::Type type, const bool fwd_dns,
                                            const bool rev_dns,
                                            const std::string& hostname,
                                            const bool fake_allocation):
    subnet_(subnet), duid_(duid), iaid_(iaid), type_(type), hwaddr_(),
    hints_(), fwd_dns_update_(fwd_dns), rev_dns_update_(rev_dns),
    hostname_(hostname), fake_allocation_(fake_allocation),
    old_leases_(), host_(), query_(), ia_rsp_(),
    allow_new_leases_in_renewals_(false) {

    static asiolink::IOAddress any("::");

    if (hint != any) {
        hints_.push_back(std::make_pair(hint, 128));
    }
    // callout_handle, host pointers initiated to NULL by their
    // respective constructors.
}



Lease6Collection
AllocEngine::allocateLeases6(ClientContext6& ctx) {

    try {
        if (!ctx.subnet_) {
            isc_throw(InvalidOperation, "Subnet is required for IPv6 lease allocation");
        } else
        if (!ctx.duid_) {
            isc_throw(InvalidOperation, "DUID is mandatory for IPv6 lease allocation");
        }

        // Check which host reservation mode is supported in this subnet.
        Subnet::HRMode hr_mode = ctx.subnet_->getHostReservationMode();

        // Check if there's a host reservation for this client. Attempt to get
        // host info only if reservations are not disabled.
        if (hr_mode != Subnet::HR_DISABLED) {

            ctx.host_ = HostMgr::instance().get6(ctx.subnet_->getID(), ctx.duid_,
                                                 ctx.hwaddr_);
        } else {
            // Let's explicitly set it to NULL if reservations are disabled.
            ctx.host_.reset();
        }

        // Check if there are existing leases for that subnet/duid/iaid
        // combination.
        Lease6Collection leases =
            LeaseMgrFactory::instance().getLeases6(ctx.type_, *ctx.duid_, ctx.iaid_,
                                                   ctx.subnet_->getID());

        // Now do the checks:
        // Case 1. if there are no leases, and there are reservations...
        //   1.1. are the reserved addresses are used by someone else?
        //       yes: we have a problem
        //       no: assign them => done
        // Case 2. if there are leases and there are no reservations...
        //   2.1 are the leases reserved for someone else?
        //       yes: release them, assign something else
        //       no: renew them => done
        // Case 3. if there are leases and there are reservations...
        //   3.1 are the leases matching reservations?
        //       yes: renew them => done
        //       no: release existing leases, assign new ones based on reservations
        // Case 4/catch-all. if there are no leases and no reservations...
        //       assign new leases
        //
        // We could implement those checks as nested ifs, but the performance
        // gain would be minimal and the code readibility loss would be substantial.
        // Hence independent checks.

        // Case 1: There are no leases and there's a reservation for this host.
        if (leases.empty() && ctx.host_) {

            // Try to allocate leases that match reservations. Typically this will
            // succeed, except cases where the reserved addresses are used by
            // someone else.
            allocateReservedLeases6(ctx, leases);

            // If we got at least one lease, we're good to go.
            if (!leases.empty()) {
                return (leases);
            }

            // If not, we'll need to continue and will eventually fall into case 4:
            // getting a regular lease. That could happen when we're processing
            // request from client X, there's a reserved address A for X, but
            // A is currently used by client Y. We can't immediately reassign A
            // from X to Y, because Y keeps using it, so X would send Decline right
            // away. Need to wait till Y renews, then we can release A, so it
            // will become available for X.

        // Case 2: There are existing leases and there are no reservations.
        //
        // There is at least one lease for this client and there are no reservations.
        // We will return these leases for the client, but we may need to update
        // FQDN information.
        } else if (!leases.empty() && !ctx.host_) {

            // Check if the existing leases are reserved for someone else.
            // If they're not, we're ok to keep using them.
            removeNonmatchingReservedLeases6(ctx, leases);

            if (!leases.empty()) {
                // Return old leases so the server can see what has changed.
                return (updateFqdnData(ctx, leases));
            }

            // If leases are empty at this stage, it means that we used to have
            // leases for this client, but we checked and those leases are reserved
            // for someone else, so we lost them. We will need to continue and
            // will finally end up in case 4 (no leases, no reservations), so we'll
            // assign something new.

        // Case 3: There are leases and there are reservations.
        } else if (!leases.empty() && ctx.host_) {

            // First, check if have leases matching reservations, and add new
            // leases if we don't have them.
            allocateReservedLeases6(ctx, leases);

            // leases now contain both existing and new leases that were created
            // from reservations.

            // Second, let's remove leases that are reserved for someone else.
            // This applies to any existing leases. This will not happen frequently,
            // but it may happen with the following chain of events:
            // 1. client A gets address X;
            // 2. reservation for client B for address X is made by a administrator;
            // 3. client A reboots
            // 4. client A requests the address (X) he got previously
            removeNonmatchingReservedLeases6(ctx, leases);

            // leases now contain existing and new leases, but we removed those
            // leases that are reserved for someone else (non-matching reserved).

            // There's one more check to do. Let's remove leases that are not
            // matching reservations, i.e. if client X has address A, but there's
            // a reservation for address B, we should release A and reassign B.
            // Caveat: do this only if we have at least one reserved address.
            removeNonreservedLeases6(ctx, leases);

            // All checks are done. Let's hope we have some leases left.

            // If we have any leases left, let's return them and we're done.
            if (!leases.empty()) {
                return (leases);
            }

            // If we don't have any leases at this stage, it means that we hit
            // one of the following cases:
            // - we have a reservation, but it's not for this IAID/ia-type and
            //   we had to return the address we were using
            // - we have a reservation for this iaid/ia-type, but the reserved
            //   address is currently used by someone else. We can't assign it
            //   yet.
            // - we had an address, but we just discovered that it's reserved for
            //   someone else, so we released it.
        }

        // Case 4/catch-all: One of the following is true:
        // - we don't have leases and there are no reservations
        // - we used to have leases, but we lost them, because they are now
        //   reserved for someone else
        // - we have a reservation, but it is not usable yet, because the address
        //   is still used by someone else
        //
        // In any case, we need to go through normal lease assignment process
        // for now. This is also a catch-all or last resort approach, when we
        // couldn't find any reservations (or couldn't use them).

        leases = allocateUnreservedLeases6(ctx);

        if (!leases.empty()) {
            return (leases);
        }

        // Unable to allocate an address, return an empty lease.
        LOG_WARN(dhcpsrv_logger, DHCPSRV_ADDRESS6_ALLOC_FAIL).arg(attempts_);

    } catch (const isc::Exception& e) {

        // Some other error, return an empty lease.
        LOG_ERROR(dhcpsrv_logger, DHCPSRV_ADDRESS6_ALLOC_ERROR).arg(e.what());
    }

    return (Lease6Collection());
}

Lease6Collection
AllocEngine::allocateUnreservedLeases6(ClientContext6& ctx) {

    AllocatorPtr allocator = getAllocator(ctx.type_);

    if (!allocator) {
        isc_throw(InvalidOperation, "No allocator specified for "
                  << Lease6::typeToText(ctx.type_));
    }

    // Check which host reservation mode is supported in this subnet.
    Subnet::HRMode hr_mode = ctx.subnet_->getHostReservationMode();

    Lease6Collection leases;

    IOAddress hint("::");
    if (!ctx.hints_.empty()) {
        /// @todo: We support only one hint for now
        hint = ctx.hints_[0].first;
    }

    // check if the hint is in pool and is available
    // This is equivalent of subnet->inPool(hint), but returns the pool
    Pool6Ptr pool = boost::dynamic_pointer_cast<
        Pool6>(ctx.subnet_->getPool(ctx.type_, hint, false));

    if (pool) {
        /// @todo: We support only one hint for now
        Lease6Ptr lease = LeaseMgrFactory::instance().getLease6(ctx.type_, hint);
        if (!lease) {

            // In-pool reservations: Check if this address is reserved for someone
            // else. There is no need to check for whom it is reserved, because if
            // it has been reserved for us we would have already allocated a lease.

            ConstHostPtr host;
            if (hr_mode != Subnet::HR_DISABLED) {
                host = HostMgr::instance().get6(ctx.subnet_->getID(), hint);
            }

            if (!host) {
                // If the in-pool reservations are disabled, or there is no
                // reservation for a given hint, we're good to go.

                // The hint is valid and not currently used, let's create a
                // lease for it
                lease = createLease6(ctx, hint, pool->getLength());

                // It can happen that the lease allocation failed (we could
                // have lost the race condition. That means that the hint is
                // no longer usable and we need to continue the regular
                // allocation path.
                if (lease) {

                    /// @todo: We support only one lease per ia for now
                    Lease6Collection collection;
                    collection.push_back(lease);
                    return (collection);
                }
            }
        } else {

            // If the lease is expired, we may likely reuse it, but...
            if (lease->expired()) {

                ConstHostPtr host;
                if (hr_mode != Subnet::HR_DISABLED) {
                    host = HostMgr::instance().get6(ctx.subnet_->getID(), hint);
                }

                // Let's check if there is a reservation for this address.
                if (!host) {

                    // Copy an existing, expired lease so as it can be returned
                    // to the caller.
                    Lease6Ptr old_lease(new Lease6(*lease));
                    ctx.old_leases_.push_back(old_lease);

                    /// We found a lease and it is expired, so we can reuse it
                    lease = reuseExpiredLease(lease, ctx, pool->getLength());

                    /// @todo: We support only one lease per ia for now
                    leases.push_back(lease);
                    return (leases);
                }
            }
        }
    }

    // The hint was useless (it was not provided at all, was used by someone else,
    // was out of pool or reserved for someone else). Search the pool until first
    // of the following occurs:
    // - we find a free address
    // - we find an address for which the lease has expired
    // - we exhaust number of tries
    //
    /// @todo: We used to use hardcoded number of attempts (100). Now we dynamically
    /// calculate the number of possible leases in all pools in this subnet and
    /// try that number of times at most. It would be useful to that value if
    /// attempts_, specified by the user could override that value (and keep
    /// dynamic if they're set to 0).
    uint32_t max_attempts = ctx.subnet_->getPoolCapacity(ctx.type_);
    for (uint32_t i = 0; i < max_attempts; ++i)
    {
        IOAddress candidate = allocator->pickAddress(ctx.subnet_, ctx.duid_, hint);

        /// In-pool reservations: Check if this address is reserved for someone
        /// else. There is no need to check for whom it is reserved, because if
        /// it has been reserved for us we would have already allocated a lease.
        if (hr_mode == Subnet::HR_ALL &&
            HostMgr::instance().get6(ctx.subnet_->getID(), candidate)) {

            // Don't allocate.
            continue;
        }

        // The first step is to find out prefix length. It is 128 for
        // non-PD leases.
        uint8_t prefix_len = 128;
        if (ctx.type_ == Lease::TYPE_PD) {
            Pool6Ptr pool = boost::dynamic_pointer_cast<Pool6>(
                ctx.subnet_->getPool(ctx.type_, candidate, false));
            /// @todo: verify that the pool is non-null
            prefix_len = pool->getLength();
        }

        Lease6Ptr existing = LeaseMgrFactory::instance().getLease6(ctx.type_,
                                                                   candidate);
        if (!existing) {

            // there's no existing lease for selected candidate, so it is
            // free. Let's allocate it.

            Lease6Ptr lease = createLease6(ctx, candidate, prefix_len);
            if (lease) {
                // We are allocating a new lease (not renewing). So, the
                // old lease should be NULL.
                ctx.old_leases_.clear();

                leases.push_back(lease);
                return (leases);
            }

            // Although the address was free just microseconds ago, it may have
            // been taken just now. If the lease insertion fails, we continue
            // allocation attempts.
        } else {
            if (existing->expired()) {
                // Copy an existing, expired lease so as it can be returned
                // to the caller.
                Lease6Ptr old_lease(new Lease6(*existing));
                ctx.old_leases_.push_back(old_lease);

                existing = reuseExpiredLease(existing,
                                             ctx,
                                             prefix_len);

                leases.push_back(existing);
                return (leases);
            }
        }
    }

    // We failed to allocate anything. Let's return empty collection.
    return (Lease6Collection());
}

void
AllocEngine::allocateReservedLeases6(ClientContext6& ctx, Lease6Collection& existing_leases) {

    // If there are no reservations or the reservation is v4, there's nothing to do.
    if (!ctx.host_ || !ctx.host_->hasIPv6Reservation()) {
        return;
    }

    // Let's convert this from Lease::Type to IPv6Reserv::Type
    IPv6Resrv::Type type = ctx.type_ == Lease::TYPE_NA ? IPv6Resrv::TYPE_NA : IPv6Resrv::TYPE_PD;

    // Get the IPv6 reservations of specified type.
    const IPv6ResrvRange& reservs = ctx.host_->getIPv6Reservations(type);

    if (std::distance(reservs.first, reservs.second) == 0) {
        // No reservations? We're done here.
        return;
    }

    for (IPv6ResrvIterator resv = reservs.first; resv != reservs.second; ++resv) {
        // We do have a reservation for addr.
        IOAddress addr = resv->second.getPrefix();
        uint8_t prefix_len = resv->second.getPrefixLen();

        // Check if already have this lease on the existing_leases list.
        for (Lease6Collection::const_iterator l = existing_leases.begin();
             l != existing_leases.end(); ++l) {

            // Ok, we already have a lease for this reservation and it's usable
            if (((*l)->addr_ == addr) && (*l)->valid_lft_ != 0) {
                return;
            }
        }

        // If there's a lease for this address, let's not create it.
        // It doesn't matter whether it is for this client or for someone else.
        if (!LeaseMgrFactory::instance().getLease6(ctx.type_, addr)) {
            // Ok, let's create a new lease...
            Lease6Ptr lease = createLease6(ctx, addr, prefix_len);

            // ... and add it to the existing leases list.
            existing_leases.push_back(lease);

            if (ctx.type_ == Lease::TYPE_NA) {
                LOG_INFO(dhcpsrv_logger, DHCPSRV_HR_RESERVED_ADDR_GRANTED)
                    .arg(addr.toText()).arg(ctx.duid_->toText());
            } else {
                LOG_INFO(dhcpsrv_logger, DHCPSRV_HR_RESERVED_PREFIX_GRANTED)
                    .arg(addr.toText()).arg(static_cast<int>(prefix_len))
                    .arg(ctx.duid_->toText());
            }

            // We found a lease for this client and this IA. Let's return.
            // Returning after the first lease was assigned is useful if we
            // have multiple reservations for the same client. If the client
            // sends 2 IAs, the first time we call allocateReservedLeases6 will
            // use the first reservation and return. The second time, we'll
            // go over the first reservation, but will discover that there's
            // a lease corresponding to it and will skip it and then pick
            // the second reservation and turn it into the lease. This approach
            // would work for any number of reservations.
            return;
        }
    }
}

void
AllocEngine::removeNonmatchingReservedLeases6(ClientContext6& ctx,
                                              Lease6Collection& existing_leases) {
    // If there are no leases (so nothing to remove) or
    // host reservation is disabled (so there are no reserved leases),
    // just return.
    if (existing_leases.empty() || !ctx.subnet_ ||
        (ctx.subnet_->getHostReservationMode() == Subnet::HR_DISABLED) ) {
        return;
    }

    // We need a copy, so we won't be iterating over a container and
    // removing from it at the same time. It's only a copy of pointers,
    // so the operation shouldn't be that expensive.
    Lease6Collection copy = existing_leases;

    for (Lease6Collection::const_iterator candidate = copy.begin();
         candidate != copy.end(); ++candidate) {

        ConstHostPtr host = HostMgr::instance().get6(ctx.subnet_->getID(),
                                                     (*candidate)->addr_);

        if (!host || (host == ctx.host_)) {
            // Not reserved or reserved for us. That's ok, let's check
            // the next lease.
            continue;
        }

        // Ok, we have a problem. This host has a lease that is reserved
        // for someone else. We need to recover from this.
        if (ctx.type_ == Lease::TYPE_NA) {
            LOG_INFO(dhcpsrv_logger, DHCPSRV_HR_REVOKED_ADDR6_LEASE)
                .arg((*candidate)->addr_.toText()).arg(ctx.duid_->toText())
                .arg(host->getIdentifierAsText());
        } else {
            LOG_INFO(dhcpsrv_logger, DHCPSRV_HR_REVOKED_PREFIX6_LEASE)
                .arg((*candidate)->addr_.toText())
                .arg(static_cast<int>((*candidate)->prefixlen_))
                .arg(ctx.duid_->toText())
                .arg(host->getIdentifierAsText());
        }

        // Remove this lease from LeaseMgr
        LeaseMgrFactory::instance().deleteLease((*candidate)->addr_);

        // In principle, we could trigger a hook here, but we will do this
        // only if we get serious complaints from actual users. We want the
        // conflict resolution procedure to really work and user libraries
        // should not interfere with it.

        // Add this to the list of removed leases.
        ctx.old_leases_.push_back(*candidate);

        // Let's remove this candidate from existing leases
        removeLeases(existing_leases, (*candidate)->addr_);
    }
}

bool
AllocEngine::removeLeases(Lease6Collection& container, const asiolink::IOAddress& addr) {

    bool removed = false;
    for (Lease6Collection::iterator lease = container.begin();
         lease != container.end(); ++lease) {
        if ((*lease)->addr_ == addr) {
            lease->reset();
            removed = true;
        }
    }

    // Remove all elements that have NULL value
    container.erase(std::remove(container.begin(), container.end(), Lease6Ptr()),
                    container.end());

    return (removed);
}

void
AllocEngine::removeNonreservedLeases6(ClientContext6& ctx,
                                      Lease6Collection& existing_leases) {
    // This method removes leases that are not reserved for this host.
    // It will keep at least one lease, though.
    if (existing_leases.empty() || !ctx.host_ || !ctx.host_->hasIPv6Reservation()) {
        return;
    }

    // This is the total number of leases. We should not remove the last one.
    int total = existing_leases.size();

    // This is officially not scary code anymore. iterates and marks specified
    // leases for deletion, by setting appropriate pointers to NULL.
    for (Lease6Collection::iterator lease = existing_leases.begin();
         lease != existing_leases.end(); ++lease) {
        IPv6Resrv resv(ctx.type_ == Lease::TYPE_NA ? IPv6Resrv::TYPE_NA : IPv6Resrv::TYPE_PD,
                       (*lease)->addr_, (*lease)->prefixlen_);
        if (!ctx.host_->hasReservation(resv)) {
            // We have reservations, but not for this lease. Release it.

            // Remove this lease from LeaseMgr
            LeaseMgrFactory::instance().deleteLease((*lease)->addr_);

            /// @todo: Probably trigger a hook here

            // Add this to the list of removed leases.
            ctx.old_leases_.push_back(*lease);

            // Set this pointer to NULL. The pointer is still valid. We're just
            // setting the Lease6Ptr to NULL value. We'll remove all NULL
            // pointers once the loop is finished.
            lease->reset();

            if (--total == 1) {
                // If there's only one lease left, break the loop.
                break;
            }
        }

    }

    // Remove all elements that we previously marked for deletion (those that
    // have NULL value).
    existing_leases.erase(std::remove(existing_leases.begin(),
        existing_leases.end(), Lease6Ptr()), existing_leases.end());
}

Lease6Ptr
AllocEngine::reuseExpiredLease(Lease6Ptr& expired, ClientContext6& ctx,
                               uint8_t prefix_len) {

    if (!expired->expired()) {
        isc_throw(BadValue, "Attempt to recycle lease that is still valid");
    }

    if (expired->type_ != Lease::TYPE_PD) {
        prefix_len = 128; // non-PD lease types must be always /128
    }

    // address, lease type and prefixlen (0) stay the same
    expired->iaid_ = ctx.iaid_;
    expired->duid_ = ctx.duid_;
    expired->preferred_lft_ = ctx.subnet_->getPreferred();
    expired->valid_lft_ = ctx.subnet_->getValid();
    expired->t1_ = ctx.subnet_->getT1();
    expired->t2_ = ctx.subnet_->getT2();
    expired->cltt_ = time(NULL);
    expired->subnet_id_ = ctx.subnet_->getID();
    expired->fixed_ = false;
    expired->hostname_ = ctx.hostname_;
    expired->fqdn_fwd_ = ctx.fwd_dns_update_;
    expired->fqdn_rev_ = ctx.rev_dns_update_;
    expired->prefixlen_ = prefix_len;

    /// @todo: log here that the lease was reused (there's ticket #2524 for
    /// logging in libdhcpsrv)

    // Let's execute all callouts registered for lease6_select
    if (ctx.callout_handle_ &&
        HooksManager::getHooksManager().calloutsPresent(hook_index_lease6_select_)) {

        // Delete all previous arguments
        ctx.callout_handle_->deleteAllArguments();

        // Pass necessary arguments
        // Subnet from which we do the allocation
        ctx.callout_handle_->setArgument("subnet6", ctx.subnet_);

        // Is this solicit (fake = true) or request (fake = false)
        ctx.callout_handle_->setArgument("fake_allocation", ctx.fake_allocation_);

        // The lease that will be assigned to a client
        ctx.callout_handle_->setArgument("lease6", expired);

        // Call the callouts
        HooksManager::callCallouts(hook_index_lease6_select_, *ctx.callout_handle_);

        // Callouts decided to skip the action. This means that the lease is not
        // assigned, so the client will get NoAddrAvail as a result. The lease
        // won't be inserted into the database.
        if (ctx.callout_handle_->getSkip()) {
            LOG_DEBUG(dhcpsrv_logger, DHCPSRV_DBG_HOOKS, DHCPSRV_HOOK_LEASE6_SELECT_SKIP);
            return (Lease6Ptr());
        }

        // Let's use whatever callout returned. Hopefully it is the same lease
        // we handed to it.
        ctx.callout_handle_->getArgument("lease6", expired);
    }

    if (!ctx.fake_allocation_) {
        // for REQUEST we do update the lease
        LeaseMgrFactory::instance().updateLease6(expired);
    }

    // We do nothing for SOLICIT. We'll just update database when
    // the client gets back to us with REQUEST message.

    // it's not really expired at this stage anymore - let's return it as
    // an updated lease
    return (expired);
}

Lease6Ptr AllocEngine::createLease6(ClientContext6& ctx,
                                    const IOAddress& addr,
                                    uint8_t prefix_len) {

    if (ctx.type_ != Lease::TYPE_PD) {
        prefix_len = 128; // non-PD lease types must be always /128
    }

    Lease6Ptr lease(new Lease6(ctx.type_, addr, ctx.duid_, ctx.iaid_,
                               ctx.subnet_->getPreferred(), ctx.subnet_->getValid(),
                               ctx.subnet_->getT1(), ctx.subnet_->getT2(),
                               ctx.subnet_->getID(), ctx.hwaddr_, prefix_len));

    lease->fqdn_fwd_ = ctx.fwd_dns_update_;
    lease->fqdn_rev_ = ctx.rev_dns_update_;
    lease->hostname_ = ctx.hostname_;

    // Let's execute all callouts registered for lease6_select
    if (ctx.callout_handle_ &&
        HooksManager::getHooksManager().calloutsPresent(hook_index_lease6_select_)) {

        // Delete all previous arguments
        ctx.callout_handle_->deleteAllArguments();

        // Pass necessary arguments

        // Subnet from which we do the allocation
        ctx.callout_handle_->setArgument("subnet6", ctx.subnet_);

        // Is this solicit (fake = true) or request (fake = false)
        ctx.callout_handle_->setArgument("fake_allocation", ctx.fake_allocation_);
        ctx.callout_handle_->setArgument("lease6", lease);

        // This is the first callout, so no need to clear any arguments
        HooksManager::callCallouts(hook_index_lease6_select_, *ctx.callout_handle_);

        // Callouts decided to skip the action. This means that the lease is not
        // assigned, so the client will get NoAddrAvail as a result. The lease
        // won't be inserted into the database.
        if (ctx.callout_handle_->getSkip()) {
            LOG_DEBUG(dhcpsrv_logger, DHCPSRV_DBG_HOOKS, DHCPSRV_HOOK_LEASE6_SELECT_SKIP);
            return (Lease6Ptr());
        }

        // Let's use whatever callout returned. Hopefully it is the same lease
        // we handed to it.
        ctx.callout_handle_->getArgument("lease6", lease);
    }

    if (!ctx.fake_allocation_) {
        // That is a real (REQUEST) allocation
        bool status = LeaseMgrFactory::instance().addLease(lease);

        if (status) {

            return (lease);
        } else {
            // One of many failures with LeaseMgr (e.g. lost connection to the
            // database, database failed etc.). One notable case for that
            // is that we are working in multi-process mode and we lost a race
            // (some other process got that address first)
            return (Lease6Ptr());
        }
    } else {
        // That is only fake (SOLICIT without rapid-commit) allocation

        // It is for advertise only. We should not insert the lease into LeaseMgr,
        // but rather check that we could have inserted it.
        Lease6Ptr existing = LeaseMgrFactory::instance().getLease6(
                             Lease::TYPE_NA, addr);
        if (!existing) {
            return (lease);
        } else {
            return (Lease6Ptr());
        }
    }
}

Lease6Collection
AllocEngine::renewLeases6(ClientContext6& ctx) {
    try {
        if (!ctx.subnet_) {
            isc_throw(InvalidOperation, "Subnet is required for allocation");
        }

        if (!ctx.duid_) {
            isc_throw(InvalidOperation, "DUID is mandatory for allocation");
        }

        // Check which host reservation mode is supported in this subnet.
        Subnet::HRMode hr_mode = ctx.subnet_->getHostReservationMode();

        // Check if there's a host reservation for this client. Attempt to get
        // host info only if reservations are not disabled.
        if (hr_mode != Subnet::HR_DISABLED) {

            ctx.host_ = HostMgr::instance().get6(ctx.subnet_->getID(), ctx.duid_,
                                                 ctx.hwaddr_);
        } else {
            // Host reservations disabled? Then explicitly set host to NULL
            ctx.host_.reset();
        }

        // Check if there are any leases for this client.
        Lease6Collection leases = LeaseMgrFactory::instance()
            .getLeases6(ctx.type_, *ctx.duid_, ctx.iaid_, ctx.subnet_->getID());

        if (!leases.empty()) {
            // Check if the existing leases are reserved for someone else.
            // If they're not, we're ok to keep using them.
            removeNonmatchingReservedLeases6(ctx, leases);
        }

        if (ctx.host_) {
            // If we have host reservation, allocate those leases.
            allocateReservedLeases6(ctx, leases);

            // There's one more check to do. Let's remove leases that are not
            // matching reservations, i.e. if client X has address A, but there's
            // a reservation for address B, we should release A and reassign B.
            // Caveat: do this only if we have at least one reserved address.
            removeNonreservedLeases6(ctx, leases);
        }

        // If we happen to removed all leases, get something new for this guy.
        // Depending on the configuration, we may enable or disable granting
        // new leases during renewals. This is controlled with the
        // allow_new_leases_in_renewals_ field.
        if (leases.empty() && ctx.allow_new_leases_in_renewals_) {
            leases = allocateUnreservedLeases6(ctx);
        }

        // Extend all existing leases that passed all checks.
        for (Lease6Collection::iterator l = leases.begin(); l != leases.end(); ++l) {
            extendLease6(ctx, *l);
        }

        return (leases);

    } catch (const isc::Exception& e) {

        // Some other error, return an empty lease.
        LOG_ERROR(dhcpsrv_logger, DHCPSRV_RENEW6_ERROR).arg(e.what());
    }

    return (Lease6Collection());
}

void
AllocEngine::extendLease6(ClientContext6& ctx, Lease6Ptr lease) {

    if (!lease || !ctx.subnet_) {
        return;
    }

    // Check if the lease still belongs to the subnet. If it doesn't,
    // we'll need to remove it.
    if ((lease->type_ != Lease::TYPE_PD) && !ctx.subnet_->inRange(lease->addr_)) {
        // Oh dear, the lease is no longer valid. We need to get rid of it.

        // Remove this lease from LeaseMgr
        LeaseMgrFactory::instance().deleteLease(lease->addr_);

        // Add it to the removed leases list.
        ctx.old_leases_.push_back(lease);

        return;
    }

    // Keep the old data in case the callout tells us to skip update.
    Lease6 old_data = *lease;

    lease->preferred_lft_ = ctx.subnet_->getPreferred();
    lease->valid_lft_ = ctx.subnet_->getValid();
    lease->t1_ = ctx.subnet_->getT1();
    lease->t2_ = ctx.subnet_->getT2();
    lease->cltt_ = time(NULL);
    lease->hostname_ = ctx.hostname_;
    lease->fqdn_fwd_ = ctx.fwd_dns_update_;
    lease->fqdn_rev_ = ctx.rev_dns_update_;
    lease->hwaddr_ = ctx.hwaddr_;

    bool skip = false;
    // Get the callouts specific for the processed message and execute them.
    int hook_point = ctx.query_->getType() == DHCPV6_RENEW ?
        Hooks.hook_index_lease6_renew_ : Hooks.hook_index_lease6_rebind_;
    if (HooksManager::calloutsPresent(hook_point)) {
        CalloutHandlePtr callout_handle = ctx.callout_handle_;

        // Delete all previous arguments
        callout_handle->deleteAllArguments();

        // Pass the original packet
        callout_handle->setArgument("query6", ctx.query_);

        // Pass the lease to be updated
        callout_handle->setArgument("lease6", lease);

        // Pass the IA option to be sent in response
        if (lease->type_ == Lease::TYPE_NA) {
            callout_handle->setArgument("ia_na", ctx.ia_rsp_);
        } else {
            callout_handle->setArgument("ia_pd", ctx.ia_rsp_);
        }

        // Call all installed callouts
        HooksManager::callCallouts(hook_point, *callout_handle);

        // Callouts decided to skip the next processing step. The next
        // processing step would actually renew the lease, so skip at this
        // stage means "keep the old lease as it is".
        if (callout_handle->getSkip()) {
            skip = true;
            LOG_DEBUG(dhcpsrv_logger, DHCPSRV_DBG_HOOKS,
                      DHCPSRV_HOOK_LEASE6_EXTEND_SKIP)
                .arg(ctx.query_->getName());
        }
    }

    if (!skip) {
        LeaseMgrFactory::instance().updateLease6(lease);
    } else {
        // Copy back the original date to the lease. For MySQL it doesn't make
        // much sense, but for memfile, the Lease6Ptr points to the actual lease
        // in memfile, so the actual update is performed when we manipulate
        // fields of returned Lease6Ptr, the actual updateLease6() is no-op.
        *lease = old_data;
    }
}

Lease6Collection
AllocEngine::updateFqdnData(ClientContext6& ctx, const Lease6Collection& leases) {
    Lease6Collection updated_leases;
    for (Lease6Collection::const_iterator lease_it = leases.begin();
         lease_it != leases.end(); ++lease_it) {
        Lease6Ptr lease(new Lease6(**lease_it));
        lease->fqdn_fwd_ = ctx.fwd_dns_update_;
        lease->fqdn_rev_ = ctx.rev_dns_update_;
        lease->hostname_ = ctx.hostname_;
        if (!ctx.fake_allocation_ &&
            ((lease->fqdn_fwd_ != (*lease_it)->fqdn_fwd_) ||
             (lease->fqdn_rev_ != (*lease_it)->fqdn_rev_) ||
             (lease->hostname_ != (*lease_it)->hostname_))) {
            ctx.changed_leases_.push_back(*lease_it);
            LeaseMgrFactory::instance().updateLease6(lease);
        }
        updated_leases.push_back(lease);
    }
    return (updated_leases);
}

} // end of isc::dhcp namespace
} // end of isc namespace

// ##########################################################################
// #    DHCPv4 lease allocation code starts here.
// ##########################################################################

namespace {

/// @brief Check if the specific address is reserved for another client.
///
/// This function uses the HW address from the context to check if the
/// requested address (specified as first parameter) is reserved for
/// another client, i.e. client using a different HW address.
///
/// @param address An address for which the function should check if
/// there is a reservation for the different client.
/// @param ctx Client context holding the data extracted from the
/// client's message.
///
/// @return true if the address is reserved for another client.
bool
addressReserved(const IOAddress& address, const AllocEngine::ClientContext4& ctx) {
    ConstHostPtr host = HostMgr::instance().get4(ctx.subnet_->getID(), address);
    HWAddrPtr host_hwaddr;
    if (host) {
        host_hwaddr = host->getHWAddress();
        if (ctx.hwaddr_ && host_hwaddr) {
            /// @todo Use the equality operators for HWAddr class.
            /// Currently, this is impossible because the HostMgr uses the
            /// HTYPE_ETHER type, whereas the unit tests may use other types
            /// which HostMgr doesn't support yet.
            return (host_hwaddr->hwaddr_ != ctx.hwaddr_->hwaddr_);

        } else {
            return (false);

        }
    }
    return (false);
}

/// @brief Check if the context contains the reservation for the
/// IPv4 address.
///
/// This convenience function checks if the context contains the reservation
/// for the IPv4 address. Note that some reservations may not assign a
/// static IPv4 address to the clients, but may rather reserve a hostname.
/// Allocation engine should check if the existing reservation is made
/// for the IPv4 address and if it is not, allocate the address from the
/// dynamic pool. The allocation engine uses this function to check if
/// the reservation is made for the IPv4 address.
///
/// @param ctx Client context holding the data extracted from the
/// client's message.
///
/// @return true if the context contains the reservation for the IPv4 address.
bool
hasAddressReservation(const AllocEngine::ClientContext4& ctx) {
    return (ctx.host_ && !ctx.host_->getIPv4Reservation().isV4Zero());
}

/// @brief Check if there is a lease for the client which message is processed.
///
/// This function searches the lease database to find existing lease for the client.
/// It finds the lease using the client's HW address first. If the lease exists and
/// appears to belong to the client the lease is returned. Otherwise, the function
/// will search for the lease using the client identifier (if supplied). If the
/// lease exists and appears to belong to the client, it is returned.
///
/// This function also identifies the conflicts between existing leases and the
/// lease to be allocated for the client, when the client is using a HW address
/// or client identifier which is already in use by the client having a lease in
/// the database. If the client uses an identifier which is already used by another
/// client and no other unique identifier which could be used to identify the client's
/// lease this function signals the conflict by returning 'true'.
///
/// @param ctx Client context.
/// @param [out] client_lease Client's lease found in the database.
///
/// @return true if there is a conflict of identifiers (HW address or client id)
/// between the client which message is being processed and the client which has
/// a lease in the database. When the value 'true' is returned, the caller should
/// cease the lease allocation for the client.
bool matchClientLease(const AllocEngine::ClientContext4& ctx, Lease4Ptr& client_lease) {
    // Obtain the sole instance of the LeaseMgr.
    LeaseMgr& lease_mgr = LeaseMgrFactory::instance();

    // The server should hand out existing lease to the client, so we have to check
    // if there is one. First, try to use the client's HW address.
    client_lease = lease_mgr.getLease4(*ctx.hwaddr_, ctx.subnet_->getID());
    // If there is no lease for this HW address or the lease doesn't seem to be ours,
    // we will have to use the client identifier. Note that in some situations two
    // clients may use the same HW address so even if we find the lease for the HW
    // address it doesn't mean it is ours, because client identifier may not match.
    if (ctx.clientid_ && ((!client_lease) || (client_lease && !ctx.myLease(*client_lease)))) {
        // Check if the lease is in conflict with the lease that we want to allocate.
        // If the lease is in conflict because of using overlapping HW address or
        // client identifier, we can't allocate the lease for this client.
        if (client_lease && ctx.isInConflict(*client_lease)) {
            return (true);
        }
        // There is no lease or the lease we found is not conflicting with the lease
        // which we have found for the HW address, so there is still a chance that
        // we will allocate the lease. Check if there is a lease using the client
        // identifier.
        client_lease = lease_mgr.getLease4(*ctx.clientid_, ctx.subnet_->getID());
    }

    // Check if the lease we have found belongs to us.
    if (client_lease && !ctx.myLease(*client_lease)) {
        // If the lease doesn't belong to us, check if we can add new lease for
        // the client which message we're processing, or its identifiers are
        // in conflict with this lease.
        if (ctx.isInConflict(*client_lease)) {
            return (true);
        }
        // If there is no conflict we can proceed and try to find the appropriate
        // lease but we don't use the one we found, because it is assigned to
        // someone else. Reset the pointer to indicate that we're not
        // renewing this lease.
        client_lease.reset();
    }

    return (false);
}

} // end of anonymous namespace

namespace isc {
namespace dhcp {

AllocEngine::ClientContext4::ClientContext4()
    : subnet_(), clientid_(), hwaddr_(),
      requested_address_(IOAddress::IPV4_ZERO_ADDRESS()),
      fwd_dns_update_(false), rev_dns_update_(false),
      hostname_(""), callout_handle_(), fake_allocation_(false),
      old_lease_(), host_(), conflicting_lease_() {
}

AllocEngine::ClientContext4::ClientContext4(const Subnet4Ptr& subnet,
                                            const ClientIdPtr& clientid,
                                            const HWAddrPtr& hwaddr,
                                            const asiolink::IOAddress& requested_addr,
                                            const bool fwd_dns_update,
                                            const bool rev_dns_update,
                                            const std::string& hostname,
                                            const bool fake_allocation)
    : subnet_(subnet), clientid_(clientid), hwaddr_(hwaddr),
      requested_address_(requested_addr),
      fwd_dns_update_(fwd_dns_update), rev_dns_update_(rev_dns_update),
      hostname_(hostname), callout_handle_(),
      fake_allocation_(fake_allocation), old_lease_(), host_() {
}



bool
AllocEngine::ClientContext4::myLease(const Lease4& lease) const {
    if ((!hwaddr_ && lease.hwaddr_) || (hwaddr_ && !lease.hwaddr_)) {
        return (false);
    }

    if ((hwaddr_ && lease.hwaddr_) && (hwaddr_->hwaddr_ != lease.hwaddr_->hwaddr_)) {
        return (false);
    }

    if ((!clientid_ && lease.client_id_) || (clientid_ && !lease.client_id_)) {
        return (false);
    }

    if ((clientid_ && lease.client_id_) && (*clientid_ != *lease.client_id_)) {
        return (false);
    }

    return (true);
}

bool
AllocEngine::ClientContext4::isInConflict(const Lease4& lease) const {
    return ((!(hwaddr_ && lease.hwaddr_) && (clientid_ && lease.client_id_) &&
             (*clientid_ == *lease.client_id_)) ||
            (!(clientid_ && lease.client_id_) && (hwaddr_ && lease.hwaddr_) &&
             (hwaddr_->hwaddr_ == lease.hwaddr_->hwaddr_)));
}

Lease4Ptr
AllocEngine::allocateLease4(ClientContext4& ctx) {
    // The NULL pointer indicates that the old lease didn't exist. It may
    // be later set to non NULL value if existing lease is found in the
    // database.
    ctx.old_lease_.reset();

    Lease4Ptr new_lease;

    try {
        if (!ctx.subnet_) {
            isc_throw(BadValue, "Can't allocate IPv4 address without subnet");
        }

        if (!ctx.hwaddr_) {
            isc_throw(BadValue, "HWAddr must be defined");
        }

        new_lease = ctx.fake_allocation_ ? discoverLease4(ctx) : requestLease4(ctx);
        if (!new_lease) {
            // Unable to allocate an address, return an empty lease.
            LOG_WARN(dhcpsrv_logger, DHCPSRV_ADDRESS4_ALLOC_FAIL).arg(attempts_);
        }

    } catch (const isc::Exception& e) {
        // Some other error, return an empty lease.
        LOG_ERROR(dhcpsrv_logger, DHCPSRV_ADDRESS4_ALLOC_ERROR).arg(e.what());
    }

    return (new_lease);
}

void
AllocEngine::findReservation(ClientContext4& ctx) {
    ctx.host_.reset();

    // We can only search for the reservation if a subnet has been selected.
    if (ctx.subnet_) {
        // Check which host reservation mode is supported in this subnet.
        Subnet::HRMode hr_mode = ctx.subnet_->getHostReservationMode();

        // Check if there is a host reseravtion for this client. Attempt to
        // get host information
        if (hr_mode != Subnet::HR_DISABLED) {
            // This method should handle the case when there is neither hwaddr
            // nor clientid_ available and simply return NULL.
            ctx.host_ = HostMgr::instance().get4(ctx.subnet_->getID(), ctx.hwaddr_,
                                                 ctx.clientid_);
        }
    }
}

Lease4Ptr
AllocEngine::discoverLease4(AllocEngine::ClientContext4& ctx) {
    // Find an existing lease for this client. This function will return true
    // if there is a conflict with existing lease and the allocation should
    // not be continued.
    Lease4Ptr client_lease;
    bool conflict = matchClientLease(ctx, client_lease);
    if (conflict) {
        return (Lease4Ptr());
    }

    // new_lease will hold the pointer to the lease that we will offer to the
    // caller.
    Lease4Ptr new_lease;

    // Check if there is a reservation for the client. If there is, we want to
    // assign the reserved address, rather than any other one.
    if (hasAddressReservation(ctx)) {
        // If the client doesn't have a lease or the leased address is different
        // than the reserved one then let's try to allocate the reserved address.
        // Otherwise the address that the client has is the one for which it
        // has a reservation, so just renew it.
        if (!client_lease || (client_lease->addr_ != ctx.host_->getIPv4Reservation())) {
            // The call below will return a pointer to the lease for the address
            // reserved to this client, if the lease is available, i.e. is not
            // currently assigned to any other client.
            // Note that we don't remove the existing client's lease at this point
            // because this is not a real allocation, we just offer what we can
            // allocate in the DHCPREQUEST time.
            new_lease = allocateOrReuseLease4(ctx.host_->getIPv4Reservation(), ctx);
            if (!new_lease) {
                LOG_WARN(dhcpsrv_logger, DHCPSRV_DISCOVER_ADDRESS_CONFLICT)
                    .arg(ctx.host_->getIPv4Reservation().toText())
                    .arg(ctx.conflicting_lease_ ? ctx.conflicting_lease_->toText() :
                         "(no lease info)");
            }

        } else {
            new_lease = renewLease4(client_lease, ctx);
        }
    }

    // Client does not have a reservation or the allocation of the reserved
    // address has failed, probably because the reserved address is in use
    // by another client. If the client has a lease, we will check if we can
    // offer this lease to the client. The lease can't be offered in the
    // situation when it is reserved for another client or when the address
    // is not in the dynamic pool. The former may be the result of adding the
    // new reservation for the address used by this client. The latter may
    // be due to the client using the reserved out-of-the pool address, for
    // which the reservation has just been removed.
    if (!new_lease && client_lease &&
        ctx.subnet_->inPool(Lease::TYPE_V4, client_lease->addr_) &&
        !addressReserved(client_lease->addr_, ctx)) {

        new_lease = renewLease4(client_lease, ctx);
    }

    // The client doesn't have any lease or the lease can't be offered
    // because it is either reserved for some other client or the
    // address is not in the dynamic pool.
    // Let's use the client's hint (requested IP address), if the client
    // has provided it, and try to offer it. This address must not be
    // reserved for another client, and must be in the range of the
    // dynamic pool.
    if (!new_lease && !ctx.requested_address_.isV4Zero() &&
        ctx.subnet_->inPool(Lease::TYPE_V4, ctx.requested_address_) &&
        !addressReserved(ctx.requested_address_, ctx)) {

        new_lease = allocateOrReuseLease4(ctx.requested_address_, ctx);
    }

    // The allocation engine failed to allocate all of the candidate
    // addresses. We will now use the allocator to pick the address
    // from the dynamic pool.
    if (!new_lease) {
        new_lease = allocateUnreservedLease4(ctx);
    }

    // Some of the methods like reuseExpiredLease4 may set the old lease to point
    // to the lease which they remove/override. If it is not set, but we have
    // found that the client has the lease the client's lease is the one
    // to return as an old lease.
    if (!ctx.old_lease_ && client_lease) {
        ctx.old_lease_ = client_lease;
    }
    return (new_lease);
}

Lease4Ptr
AllocEngine::requestLease4(AllocEngine::ClientContext4& ctx) {
    // Find an existing lease for this client. This function will return true
    // if there is a conflict with existing lease and the allocation should
    // not be continued.
    Lease4Ptr client_lease;
    bool conflict = matchClientLease(ctx, client_lease);
    if (conflict) {
        return (Lease4Ptr());
    }

    // Obtain the sole instance of the LeaseMgr.
    LeaseMgr& lease_mgr = LeaseMgrFactory::instance();

    // When the client sends the DHCPREQUEST, it should always specify the
    // address which it is requesting or renewing. That is, the client should
    // either use the requested IP address option or set the ciaddr. However,
    // we try to be liberal and allow the clients to not specify an address
    // in which case the allocation engine will pick a suitable address
    // for the client.
    if (!ctx.requested_address_.isV4Zero()) {
        // If the client has specified an address, make sure this address
        // is not reserved for another client. If it is, stop here because
        // we can't allocate this address.
        if (addressReserved(ctx.requested_address_, ctx)) {
            return (Lease4Ptr());
        }

    } else if (hasAddressReservation(ctx)) {
        // The client hasn't specified an address to allocate, so the
        // allocation engine needs to find an appropriate address.
        // If there is a reservation for the client, let's try to
        // allocate the reserved address.
        ctx.requested_address_ = ctx.host_->getIPv4Reservation();
    }

    if (!ctx.requested_address_.isV4Zero()) {
        // There is a specific address to be allocated. Let's find out if
        // the address is in use.
        Lease4Ptr existing = LeaseMgrFactory::instance().getLease4(ctx.requested_address_);
        // If the address is in use (allocated and not expired), we check
        // if the address is in use by our client or another client.
        // If it is in use by another client, the address can't be
        // allocated.
        if (existing && !existing->expired() && !ctx.myLease(*existing)) {
            return (Lease4Ptr());
        }

        // If the client has a reservation but it is requesting a different
        // address it is possible that the client was offered this different
        // address because the reserved address is in use. We will have to
        // check if the address is in use.
        if (hasAddressReservation(ctx) &&
            (ctx.host_->getIPv4Reservation() != ctx.requested_address_)) {
            existing = LeaseMgrFactory::instance().getLease4(ctx.host_->getIPv4Reservation());
            // If the reserved address is not in use, i.e. the lease doesn't
            // exist or is expired, and the client is requesting a different
            // address, return NULL. The client should go back to the
            // DHCPDISCOVER and the reserved address will be offered.
            if (!existing || existing->expired()) {
                return (Lease4Ptr());
            }
        }

        // The use of the out-of-pool addresses is only allowed when the requested
        // address is reserved for the client. If the address is not reserved one
        // and it doesn't belong to the dynamic pool, do not allocate it.
        if ((!hasAddressReservation(ctx) ||
             (ctx.host_->getIPv4Reservation() != ctx.requested_address_)) &&
            !ctx.subnet_->inPool(Lease4::TYPE_V4, ctx.requested_address_)) {
            return (Lease4Ptr());
        }
    }

    // We have gone through all the checks, so we can now allocate the address
    // for the client.

    // If the client is requesting an address which is assigned to the client
    // let's just renew this address. Also, renew this address if the client
    // doesn't request any specific address.
    if (client_lease) {
        if ((client_lease->addr_ == ctx.requested_address_) ||
            ctx.requested_address_.isV4Zero()) {
            return (renewLease4(client_lease, ctx));
        }
    }

    // new_lease will hold the pointer to the allocated lease if we allocate
    // successfully.
    Lease4Ptr new_lease;

    // The client doesn't have the lease or it is requesting an address
    // which it doesn't have. Let's try to allocate the requested address.
    if (!ctx.requested_address_.isV4Zero()) {
        // The call below will return a pointer to the lease allocated
        // for the client if there is no lease for the requested address,
        // or the existing lease has expired. If the allocation fails,
        // e.g. because the lease is in use, we will return NULL to
        // indicate that we were unable to allocate the lease.
        new_lease = allocateOrReuseLease4(ctx.requested_address_, ctx);

    } else {

        // We will only get here if the client didn't specify which
        // address it wanted to be allocated. The allocation engine will
        // to pick the address from the dynamic pool.
        new_lease = allocateUnreservedLease4(ctx);
    }

    // If we allocated the lease for the client, but the client already had a
    // lease, we will need to return the pointer to the previous lease and
    // the previous lease needs to be removed from the lease database.
    if (new_lease && client_lease) {
        ctx.old_lease_ = Lease4Ptr(new Lease4(*client_lease));
        lease_mgr.deleteLease(client_lease->addr_);
    }

    // Return the allocated lease or NULL pointer if allocation was
    // unsuccessful.
    return (new_lease);
}

Lease4Ptr
AllocEngine::createLease4(const ClientContext4& ctx, const IOAddress& addr) {
    if (!ctx.hwaddr_) {
        isc_throw(BadValue, "Can't create a lease with NULL HW address");
    }
    if (!ctx.subnet_) {
        isc_throw(BadValue, "Can't create a lease without a subnet");
    }

    time_t now = time(NULL);

    // @todo: remove this kludge after ticket #2590 is implemented
    std::vector<uint8_t> local_copy;
    if (ctx.clientid_) {
        local_copy = ctx.clientid_->getDuid();
    }

    Lease4Ptr lease(new Lease4(addr, ctx.hwaddr_, &local_copy[0], local_copy.size(),
                               ctx.subnet_->getValid(), ctx.subnet_->getT1(),
                               ctx.subnet_->getT2(),
                               now, ctx.subnet_->getID()));

    // Set FQDN specific lease parameters.
    lease->fqdn_fwd_ = ctx.fwd_dns_update_;
    lease->fqdn_rev_ = ctx.rev_dns_update_;
    lease->hostname_ = ctx.hostname_;

    // Let's execute all callouts registered for lease4_select
    if (ctx.callout_handle_ &&
        HooksManager::getHooksManager().calloutsPresent(hook_index_lease4_select_)) {

        // Delete all previous arguments
        ctx.callout_handle_->deleteAllArguments();

        // Pass necessary arguments

        // Subnet from which we do the allocation (That's as far as we can go
        // with using SubnetPtr to point to Subnet4 object. Users should not
        // be confused with dynamic_pointer_casts. They should get a concrete
        // pointer (Subnet4Ptr) pointing to a Subnet4 object.
        Subnet4Ptr subnet4 = boost::dynamic_pointer_cast<Subnet4>(ctx.subnet_);
        ctx.callout_handle_->setArgument("subnet4", subnet4);

        // Is this solicit (fake = true) or request (fake = false)
        ctx.callout_handle_->setArgument("fake_allocation", ctx.fake_allocation_);

        // Pass the intended lease as well
        ctx.callout_handle_->setArgument("lease4", lease);

        // This is the first callout, so no need to clear any arguments
        HooksManager::callCallouts(hook_index_lease4_select_, *ctx.callout_handle_);

        // Callouts decided to skip the action. This means that the lease is not
        // assigned, so the client will get NoAddrAvail as a result. The lease
        // won't be inserted into the database.
        if (ctx.callout_handle_->getSkip()) {
            LOG_DEBUG(dhcpsrv_logger, DHCPSRV_DBG_HOOKS, DHCPSRV_HOOK_LEASE4_SELECT_SKIP);
            return (Lease4Ptr());
        }

        // Let's use whatever callout returned. Hopefully it is the same lease
        // we handled to it.
        ctx.callout_handle_->getArgument("lease4", lease);
    }

    if (!ctx.fake_allocation_) {
        // That is a real (REQUEST) allocation
        bool status = LeaseMgrFactory::instance().addLease(lease);
        if (status) {
            return (lease);
        } else {
            // One of many failures with LeaseMgr (e.g. lost connection to the
            // database, database failed etc.). One notable case for that
            // is that we are working in multi-process mode and we lost a race
            // (some other process got that address first)
            return (Lease4Ptr());
        }
    } else {
        // That is only fake (DISCOVER) allocation

        // It is for OFFER only. We should not insert the lease into LeaseMgr,
        // but rather check that we could have inserted it.
        Lease4Ptr existing = LeaseMgrFactory::instance().getLease4(addr);
        if (!existing) {
            return (lease);
        } else {
            return (Lease4Ptr());
        }
    }
}

Lease4Ptr
AllocEngine::renewLease4(const Lease4Ptr& lease,
                         AllocEngine::ClientContext4& ctx) {
    if (!lease) {
        isc_throw(BadValue, "null lease specified for renewLease4");
    }

    // Let's keep the old data. This is essential if we are using memfile
    // (the lease returned points directly to the lease4 object in the database)
    // We'll need it if we want to skip update (i.e. roll back renewal)
    /// @todo: remove this once #3083 is implemented
    Lease4 old_values = *lease;
    ctx.old_lease_.reset(new Lease4(old_values));

    // Update the lease with the information from the context.
    updateLease4Information(lease, ctx);

    bool skip = false;
    // Execute all callouts registered for lease4_renew.
    if (HooksManager::getHooksManager().
        calloutsPresent(Hooks.hook_index_lease4_renew_)) {

        // Delete all previous arguments
        ctx.callout_handle_->deleteAllArguments();

        // Subnet from which we do the allocation. Convert the general subnet
        // pointer to a pointer to a Subnet4.  Note that because we are using
        // boost smart pointers here, we need to do the cast using the boost
        // version of dynamic_pointer_cast.
        Subnet4Ptr subnet4 = boost::dynamic_pointer_cast<Subnet4>(ctx.subnet_);

        // Pass the parameters
        ctx.callout_handle_->setArgument("subnet4", subnet4);
        ctx.callout_handle_->setArgument("clientid", ctx.clientid_);
        ctx.callout_handle_->setArgument("hwaddr", ctx.hwaddr_);

        // Pass the lease to be updated
        ctx.callout_handle_->setArgument("lease4", lease);

        // Call all installed callouts
        HooksManager::callCallouts(Hooks.hook_index_lease4_renew_,
                                   *ctx.callout_handle_);

        // Callouts decided to skip the next processing step. The next
        // processing step would actually renew the lease, so skip at this
        // stage means "keep the old lease as it is".
        if (ctx.callout_handle_->getSkip()) {
            skip = true;
            LOG_DEBUG(dhcpsrv_logger, DHCPSRV_DBG_HOOKS,
                      DHCPSRV_HOOK_LEASE4_RENEW_SKIP);
        }
    }

    if (!ctx.fake_allocation_ && !skip) {
        // for REQUEST we do update the lease
        LeaseMgrFactory::instance().updateLease4(lease);
    }
    if (skip) {
        // Rollback changes (really useful only for memfile)
        /// @todo: remove this once #3083 is implemented
        *lease = old_values;
    }

    return (lease);
}

Lease4Ptr
AllocEngine::reuseExpiredLease4(Lease4Ptr& expired,
                                AllocEngine::ClientContext4& ctx) {
    if (!expired) {
        isc_throw(BadValue, "null lease specified for reuseExpiredLease");
    }

    if (!ctx.subnet_) {
        isc_throw(BadValue, "null subnet specified for the reuseExpiredLease");
    }

    updateLease4Information(expired, ctx);
    expired->fixed_ = false;

    /// @todo: log here that the lease was reused (there's ticket #2524 for
    /// logging in libdhcpsrv)

    // Let's execute all callouts registered for lease4_select
    if (ctx.callout_handle_ &&  HooksManager::getHooksManager()
        .calloutsPresent(hook_index_lease4_select_)) {

        // Delete all previous arguments
        ctx.callout_handle_->deleteAllArguments();

        // Pass necessary arguments

        // Subnet from which we do the allocation. Convert the general subnet
        // pointer to a pointer to a Subnet4.  Note that because we are using
        // boost smart pointers here, we need to do the cast using the boost
        // version of dynamic_pointer_cast.
        Subnet4Ptr subnet4 = boost::dynamic_pointer_cast<Subnet4>(ctx.subnet_);
        ctx.callout_handle_->setArgument("subnet4", subnet4);

        // Is this solicit (fake = true) or request (fake = false)
        ctx.callout_handle_->setArgument("fake_allocation",
                                         ctx.fake_allocation_);

        // The lease that will be assigned to a client
        ctx.callout_handle_->setArgument("lease4", expired);

        // Call the callouts
        HooksManager::callCallouts(hook_index_lease4_select_, *ctx.callout_handle_);

        // Callouts decided to skip the action. This means that the lease is not
        // assigned, so the client will get NoAddrAvail as a result. The lease
        // won't be inserted into the database.
        if (ctx.callout_handle_->getSkip()) {
            LOG_DEBUG(dhcpsrv_logger, DHCPSRV_DBG_HOOKS,
                      DHCPSRV_HOOK_LEASE4_SELECT_SKIP);
            return (Lease4Ptr());
        }

        // Let's use whatever callout returned. Hopefully it is the same lease
        // we handed to it.
        ctx.callout_handle_->getArgument("lease4", expired);
    }

    if (!ctx.fake_allocation_) {
        // for REQUEST we do update the lease
        LeaseMgrFactory::instance().updateLease4(expired);
    }

    // We do nothing for SOLICIT. We'll just update database when
    // the client gets back to us with REQUEST message.

    // it's not really expired at this stage anymore - let's return it as
    // an updated lease
    return (expired);
}

Lease4Ptr
AllocEngine::allocateOrReuseLease4(const IOAddress& candidate, ClientContext4& ctx) {
    ctx.conflicting_lease_.reset();

    Lease4Ptr exist_lease = LeaseMgrFactory::instance().getLease4(candidate);
    if (exist_lease) {
        if (exist_lease->expired()) {
            ctx.old_lease_ = Lease4Ptr(new Lease4(*exist_lease));
            return (reuseExpiredLease4(exist_lease, ctx));

        } else {
            // If there is a lease and it is not expired, pass this lease back
            // to the caller in the context. The caller may need to know
            // which lease we're conflicting with.
            ctx.conflicting_lease_ = exist_lease;
        }

    } else {
        return (createLease4(ctx, candidate));
    }
    return (Lease4Ptr());
}

Lease4Ptr
AllocEngine::allocateUnreservedLease4(ClientContext4& ctx) {
    Lease4Ptr new_lease;
    AllocatorPtr allocator = getAllocator(Lease::TYPE_V4);
    const uint64_t max_attempts = ctx.subnet_->getPoolCapacity(Lease::TYPE_V4);
    for (uint64_t i = 0; i < max_attempts; ++i) {
        IOAddress candidate = allocator->pickAddress(ctx.subnet_, ctx.clientid_,
                                                     ctx.requested_address_);
        // If address is not reserved for another client, try to allocate it.
        if (!addressReserved(candidate, ctx)) {
            // The call below will return the non-NULL pointer if we
            // successfully allocate this lease. This means that the
            // address is not in use by another client.
            new_lease = allocateOrReuseLease4(candidate, ctx);
            if (new_lease) {
                return (new_lease);
            }
        }
    }

    return (new_lease);
}

void
AllocEngine::updateLease4Information(const Lease4Ptr& lease,
                                     AllocEngine::ClientContext4& ctx) const {
    lease->subnet_id_ = ctx.subnet_->getID();
    lease->hwaddr_ = ctx.hwaddr_;
    lease->client_id_ = ctx.clientid_;
    lease->cltt_ = time(NULL);
    lease->t1_ = ctx.subnet_->getT1();
    lease->t2_ = ctx.subnet_->getT2();
    lease->valid_lft_ = ctx.subnet_->getValid();
    lease->fqdn_fwd_ = ctx.fwd_dns_update_;
    lease->fqdn_rev_ = ctx.rev_dns_update_;
    lease->hostname_ = ctx.hostname_;
}

}; // end of isc::dhcp namespace
}; // end of isc namespace
