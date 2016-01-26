// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __PROCESS_ADDRESS_HPP__
#define __PROCESS_ADDRESS_HPP__

#include <stdint.h>
#ifndef __WINDOWS__
#include <unistd.h>
#endif // __WINDOWS__

#ifndef __WINDOWS__
#include <arpa/inet.h>
#include <netinet/in.h>
#endif // __WINDOWS__

#include <glog/logging.h>

#include <ostream>

#include <boost/functional/hash.hpp>

#include <stout/abort.hpp>
#include <stout/ip.hpp>
#include <stout/net.hpp>
#include <stout/stringify.hpp>

namespace process {
namespace network {

// Represents a network "address", subsuming the struct addrinfo and
// struct sockaddr* that typically is used to encapsulate IP and port.
//
// TODO(benh): Create a Family enumeration to replace sa_family_t.
// TODO(jieyu): Move this class to stout.
class Address
{
public:
  Address() : ip(INADDR_ANY), port(0) {}

  Address(const net::IP& _ip, uint16_t _port) : ip(_ip), port(_port) {}

  Address(const struct sockaddr_in& addr):
    ip(addr.sin_addr),
    port(ntohs(addr.sin_port))
  {}

  Address(const struct sockaddr_in6& addr):
    ip(addr.sin6_addr),
    port(ntohs(addr.sin6_port))
  {}

  static Address LOCALHOST_ANY()
  {
    return Address(net::IP(INADDR_ANY), 0);
  }

  static Address LOCALHOST_ANY6()
  {
    return Address(net::IP(in6addr_any), 0);
  }

  static Try<Address> create(const struct sockaddr& addr)
  {
    switch (addr.sa_family) {
       case AF_INET: {
         const struct sockaddr_in& addr4 =
            reinterpret_cast<const struct sockaddr_in&>(addr);
         return Address(addr4);
       }
       case AF_INET6: {
          const struct sockaddr_in6& addr6 =
              reinterpret_cast<const struct sockaddr_in6&>(addr);
          return Address(addr6);
       }
       default: {
         return Error(
             "Unsupported family type: " +
             stringify(addr.sa_family));
       }
     }
  }

  static Try<Address> create(const struct sockaddr_storage& storage)
  {
    return Address::create(reinterpret_cast<const struct sockaddr&>(storage));
  }

  int family() const
  {
    return ip.family();
  }

  /**
   * Returns the hostname of this address's IP.
   *
   * @returns the hostname of this address's IP.
   */
  // TODO(jmlvanre): Consider making this return a Future in order to
  // deal with slow name resolution.
  Try<std::string> hostname() const
  {
    const Try<std::string> hostname = ip.isAny()
      ? net::hostname()
      : net::getHostname(ip);

    if (hostname.isError()) {
      return Error(hostname.error());
    }

    return hostname.get();
  }

  // Returns the storage size (i.e., either sizeof(sockaddr_in) or
  // sizeof(sockaddr_in6) depending on the family) of this address.
  size_t size() const
  {
    switch (family()) {
      case AF_INET:
        return sizeof(sockaddr_in);
      case AF_INET6:
        return sizeof(sockaddr_in6);
      default:
        ABORT("Unsupported family type: " + stringify(family()));
    }
  }

  bool operator<(const Address& that) const
  {
    if (ip == that.ip) {
      return port < that.port;
    } else {
      return ip < that.ip;
    }
  }

  bool operator>(const Address& that) const
  {
    if (ip == that.ip) {
      return port > that.port;
    } else {
      return ip > that.ip;
    }
  }

  bool operator==(const Address& that) const
  {
    return (ip == that.ip && port == that.port);
  }

  bool operator!=(const Address& that) const
  {
    return !(*this == that);
  }

  net::IP ip;
  uint16_t port;
};


inline std::ostream& operator<<(std::ostream& stream, const Address& address)
{
  switch(address.family()) {
    case AF_INET:
      stream << address.ip << ":" << address.port;
      break;
    case AF_INET6:
      stream << "[" << address.ip << "]:" << address.port;
      break;
    default:
      UNREACHABLE();
  }

  return stream;
}

} // namespace network {
} // namespace process {

namespace std {

template <>
struct hash<process::network::Address>
{
  typedef size_t result_type;

  typedef process::network::Address argument_type;

  result_type operator()(const argument_type& address) const
  {
    size_t seed = 0;
    boost::hash_combine(seed, std::hash<net::IP>()(address.ip));
    boost::hash_combine(seed, address.port);
    return seed;
  }
};

} // namespace std {

#endif // __PROCESS_ADDRESS_HPP__
