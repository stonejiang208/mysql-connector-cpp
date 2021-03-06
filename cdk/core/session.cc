/*
 * Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.
 *
 * This code is licensed under the terms of the GPLv2
 * <http://www.gnu.org/licenses/old-licenses/gpl-2.0.html>, like most
 * MySQL Connectors. There are special exceptions to the terms and
 * conditions of the GPLv2 as it is applied to this software, see the
 * FLOSS License Exception
 * <http://www.mysql.com/about/legal/licensing/foss-exception.html>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
 */


#include <mysql/cdk/session.h>
#include <mysql/cdk/mysqlx/session.h>


namespace cdk {

/*
  A class that creates a session from given data source.

  Instances of this calss are callable objects which can be used as visitors
  for ds::Multi_source implementing in this case the failover logic.
*/
struct Session_builder
{
  using TLS   = cdk::connection::TLS;
  using TCPIP = cdk::connection::TCPIP;
  using Socket_base = foundation::connection::Socket_base;

  cdk::api::Connection *m_conn = NULL;
  mysqlx::Session      *m_sess = NULL;
  const mysqlx::string *m_database = NULL;
  bool m_throw_errors = false;
  scoped_ptr<Error>     m_error;
  unsigned              m_attempts = 0;

  Session_builder(bool throw_errors = false)
    : m_throw_errors(throw_errors)
  {}

  /*
    Construct a session for a given data source, if possible.

    1. If session could be constructed returns true. In this case m_sess points
       at the newly created session and m_conn points at the connection object
       used for that session. Someone must take ownership of these objects.

    2. If a network error was detected while creating session, either throws
       error if m_throw_errors is true or returns false. In the latter case,
       if used as a visitor over list of data sources, it will be called again
       to try another data source.

    3. If a bail-out error was detected, throws that error.
  */

  template <class Conn>
  Conn* connect(Conn*);

  bool operator() (const ds::TCPIP &ds, const ds::TCPIP::Options &options);
#ifndef WIN32
  bool operator() (const ds::Unix_socket&ds, const ds::Unix_socket::Options &options);
#endif
  bool operator() (const ds::TCPIP_old &ds, const ds::TCPIP_old::Options &options);

#ifdef WITH_SSL
  TLS* tls_connect(Socket_base &conn, const TLS::Options &opt);
#endif
};


template <class Conn>
Conn* Session_builder::connect(Conn* connection)
{
  m_attempts++;

  assert(connection);

  try
  {
    connection->connect();
  }
  catch (...)
  {
    delete connection;

    // Use rethrow_error() to wrap arbitrary exception in cdk::Error.

    try {
      rethrow_error();
    }
    catch (Error &err)
    {
      error_code code = err.code();

      if (m_throw_errors ||
        code == cdkerrc::auth_failure ||
        code == cdkerrc::protobuf_error ||
        code == cdkerrc::tls_error)
        throw;

      m_error.reset(err.clone());
    }

    return NULL;
  }
  return connection;
}


bool
Session_builder::operator() (
  const ds::TCPIP &ds,
  const ds::TCPIP::Options &options
  )
{
  using foundation::connection::TCPIP;
  using foundation::connection::Socket_base;

  TCPIP* connection = connect(new TCPIP(ds.host(), ds.port()));

  if (!connection)
    return false;  // continue to next host if available

#ifdef WITH_SSL
  TLS *tls_conn = tls_connect(*connection, options.get_tls());
  if (tls_conn)
  {
    m_conn = tls_conn;
    m_sess = new mysqlx::Session(*tls_conn, options);
  }
  else
#endif
  {
    m_conn = connection;
    m_sess = new mysqlx::Session(*connection, options);
  }

  m_database = options.database();
  return true;
}

#ifndef WIN32
bool
Session_builder::operator() (
  const ds::Unix_socket &ds,
  const ds::Unix_socket::Options &options
  )
{
  using foundation::connection::Unix_socket;
  using foundation::connection::Socket_base;

  Unix_socket* connection = connect(new Unix_socket(ds.path()));

  if (!connection)
    return false;  // continue to next host if available

  m_conn = connection;
  m_sess = new mysqlx::Session(*connection, options);

  m_database = options.database();

  return true;
}

#endif //#ifndef WIN32


bool
Session_builder::operator() (
  const ds::TCPIP_old &ds,
  const ds::TCPIP_old::Options &options
)
{
  throw Error(cdkerrc::generic_error, "Not supported");
  return false;
}


#ifdef WITH_SSL

Session_builder::TLS*
Session_builder::tls_connect(Socket_base &connection, const TLS::Options &options)
{
  if (!options.get_ca().empty() &&
      options.ssl_mode() < TLS::Options::SSL_MODE::VERIFY_CA)
    throw Error(cdkerrc::generic_error,
                "ssl-ca set and ssl-mode different than VERIFY_CA or VERIFY_IDENTITY");

  if (options.ssl_mode() >= TLS::Options::SSL_MODE::VERIFY_CA &&
      options.get_ca().empty())
    throw Error(cdkerrc::generic_error,
                "Missing ssl-ca option to verify CA");

  if (options.ssl_mode() == TLS::Options::SSL_MODE::DISABLED)
    return NULL;

  // Negotiate TLS capabilities.

  cdk::protocol::mysqlx::Protocol proto(connection);

  struct : cdk::protocol::mysqlx::api::Any::Document
  {
    void process(Processor &prc) const
    {
      prc.doc_begin();
      cdk::safe_prc(prc)->key_val("tls")->scalar()->yesno(true);
      prc.doc_end();
    }
  } tls_caps;

  proto.snd_CapabilitiesSet(tls_caps).wait();

  struct : cdk::protocol::mysqlx::Reply_processor
  {
    bool m_tls;
    bool m_fallback;  // fallback to plain connection if TLS not available?

    void error(unsigned int code, short int severity,
      cdk::protocol::mysqlx::sql_state_t sql_state, const string &msg)
    {
      sql_state_t expected_state("HY000");

      if (code == 5001 &&
          severity == 2 &&
          expected_state == sql_state && m_fallback)
      {
        m_tls = false;
      }
      else
      {
        throw Error(static_cast<int>(code), msg);
      }
    }
  } prc;

  prc.m_tls = true;
  prc.m_fallback = (TLS::Options::SSL_MODE::PREFERRED == options.ssl_mode());

  proto.rcv_Reply(prc).wait();

  if (!prc.m_tls)
    return NULL;

  // Capabilites OK, create TLS connection now.

  TLS *tls_conn = new TLS(&connection, options);

  // TODO: attempt failover if TLS-layer reports network error?
  tls_conn->connect();

  return tls_conn;
}

#endif


Session::Session(ds::TCPIP &ds, const ds::TCPIP::Options &options)
  : m_session(NULL)
  , m_connection(NULL)
  , m_trans(false)
{
  Session_builder sb(true);  // throw errors if detected

  sb(ds, options);

  assert(sb.m_sess);

  m_session = sb.m_sess;
  m_connection = sb.m_conn;
}


struct ds::Multi_source::Access
{
  template <class Visitor>
  static void visit(Multi_source &ds, Visitor &visitor)
  { ds.visit(visitor); }
};


Session::Session(ds::Multi_source &ds)
  : m_session(NULL)
  , m_connection(NULL)
  , m_trans(false)
{
  Session_builder sb;

  ds::Multi_source::Access::visit(ds, sb);

  if (!sb.m_sess)
  {
    if (1 == sb.m_attempts && sb.m_error)
      sb.m_error->rethrow();
    else
      throw_error(
        1 == sb.m_attempts ?
        "Could not connect to the given data source" :
        "Could not connect ot any of the given data sources"
      );
  }

  m_session = sb.m_sess;
  m_database = sb.m_database;
  m_connection = sb.m_conn;
}


#ifndef WIN32
Session::Session(ds::Unix_socket &ds, const ds::Unix_socket::Options &options)
  : m_session(NULL)
  , m_connection(NULL)
  , m_trans(false)
{
  Session_builder sb(true);  // throw errors if detected

  sb(ds, options);

  assert(sb.m_sess);

  m_session = sb.m_sess;
  m_connection = sb.m_conn;
}
#endif //#ifndef WIN32


Session::~Session()
{
  if (m_trans)
    rollback();
  delete m_session;
  delete m_connection;
}


} //cdk
