// Copyright (C) 2011  Internet Systems Consortium, Inc. ("ISC")
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

#ifndef __ASIOLINK_RECURSIVE_QUERY_H
#define __ASIOLINK_RECURSIVE_QUERY_H 1

#include <asiolink/dns_service.h>
#include <asiolink/dns_server.h>
#include <dns/buffer.h>
#include <cache/resolver_cache.h>

namespace asiolink {
/// \brief The \c RecursiveQuery class provides a layer of abstraction around
/// the ASIO code that carries out an upstream query.
///
/// This design is very preliminary; currently it is only capable of
/// handling simple forward requests to a single resolver.
class RecursiveQuery {
    ///
    /// \name Constructors
    ///
    //@{
public:
    /// \brief Constructor
    ///
    /// This is currently the only way to construct \c RecursiveQuery
    /// object. If the addresses of the forward nameservers is specified,
    /// and every upstream query will be sent to one random address, and
    /// the result sent back directly. If not, it will do full resolving.
    ///
    /// \param dns_service The DNS Service to perform the recursive
    ///        query on.
    /// \param upstream Addresses and ports of the upstream servers
    ///        to forward queries to.
    /// \param upstream_root Addresses and ports of the root servers
    ///        to use when resolving.
    /// \param query_timeout Timeout value for queries we sent, in ms
    /// \param client_timeout Timeout value for when we send back an
    ///        error, in ms
    /// \param lookup_timeout Timeout value for when we give up, in ms
    /// \param retries how many times we try again (0 means just send and
    ///     and return if it returs).
    RecursiveQuery(DNSService& dns_service,
                   const std::vector<std::pair<std::string, uint16_t> >&
                   upstream, 
                   const std::vector<std::pair<std::string, uint16_t> >&
                   upstream_root, 
                   int query_timeout = 2000,
                   int client_timeout = 4000,
                   int lookup_timeout = 30000,
                   unsigned retries = 3);
    //@}

    /// \brief Initiate resolving
    /// 
    /// When sendQuery() is called, a (set of) message(s) is sent
    /// asynchronously. If upstream servers are set, one is chosen
    /// and the response (if any) from that server will be returned.
    ///
    /// If not upstream is set, a root server is chosen from the
    /// root_servers, and the RunningQuery shall do a full resolve
    /// (i.e. if the answer is a delegation, it will be followed, etc.)
    /// until there is an answer or an error.
    ///
    /// When there is a response or an error and we give up, the given
    /// CallbackPtr object shall be called (with either success() or
    /// failure(). See ResolverInterface::Callback for more information.
    ///
    /// \param question The question being answered <qname/qclass/qtype>
    /// \param callback Callback object. See
    ///        \c ResolverInterface::Callback for more information
    void resolve(const isc::dns::QuestionPtr& question,
                 const isc::resolve::ResolverInterface::CallbackPtr callback);


    /// \brief Initiates resolving for the given question.
    ///
    /// This actually calls the previous sendQuery() with a default
    /// callback object, which calls resume() on the given DNSServer
    /// object.
    ///
    /// \param question The question being answered <qname/qclass/qtype>
    /// \param answer_message An output Message into which the final response will be copied
    /// \param buffer An output buffer into which the intermediate responses will be copied
    /// \param server A pointer to the \c DNSServer object handling the client
    void resolve(const isc::dns::Question& question,
                 isc::dns::MessagePtr answer_message,
                 isc::dns::OutputBufferPtr buffer,
                 DNSServer* server);
private:
    DNSService& dns_service_;
    boost::shared_ptr<std::vector<std::pair<std::string, uint16_t> > >
        upstream_;
    boost::shared_ptr<std::vector<std::pair<std::string, uint16_t> > >
        upstream_root_;
    int query_timeout_;
    int client_timeout_;
    int lookup_timeout_;
    unsigned retries_;
    // Cache. TODO: I think we want this initialized in Resolver class,
    // not here
    isc::cache::ResolverCache cache_;
};

}      // namespace asiolink
#endif // __ASIOLINK_RECURSIVE_QUERY_H