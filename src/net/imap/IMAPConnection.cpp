//
// VMime library (http://www.vmime.org)
// Copyright (C) 2002-2005 Vincent Richard <vincent@vincent-richard.net>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of
// the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//

#include "vmime/net/imap/IMAPTag.hpp"
#include "vmime/net/imap/IMAPConnection.hpp"
#include "vmime/net/imap/IMAPUtils.hpp"
#include "vmime/net/imap/IMAPStore.hpp"

#include "vmime/exception.hpp"
#include "vmime/platformDependant.hpp"

#if VMIME_HAVE_SASL_SUPPORT
	#include "vmime/security/sasl/SASLContext.hpp"
#endif // VMIME_HAVE_SASL_SUPPORT

#include <sstream>


// Helpers for service properties
#define GET_PROPERTY(type, prop) \
	(m_store->getInfos().getPropertyValue <type>(getSession(), \
		dynamic_cast <const IMAPStore::_infos&>(m_store->getInfos()).getProperties().prop))
#define HAS_PROPERTY(prop) \
	(m_store->getInfos().hasProperty(getSession(), \
		dynamic_cast <const IMAPStore::_infos&>(m_store->getInfos()).getProperties().prop))


namespace vmime {
namespace net {
namespace imap {


IMAPConnection::IMAPConnection(weak_ref <IMAPStore> store, ref <security::authenticator> auth)
	: m_store(store), m_auth(auth), m_socket(NULL), m_parser(NULL), m_tag(NULL),
	  m_hierarchySeparator('\0'), m_state(STATE_NONE), m_timeoutHandler(NULL)
{
}


IMAPConnection::~IMAPConnection()
{
	try
	{
		if (isConnected())
			disconnect();
		else if (m_socket)
			internalDisconnect();
	}
	catch (vmime::exception&)
	{
		// Ignore
	}
}


void IMAPConnection::connect()
{
	if (isConnected())
		throw exceptions::already_connected();

	m_state = STATE_NONE;
	m_hierarchySeparator = '\0';

	const string address = GET_PROPERTY(string, PROPERTY_SERVER_ADDRESS);
	const port_t port = GET_PROPERTY(port_t, PROPERTY_SERVER_PORT);

	// Create the time-out handler
	if (HAS_PROPERTY(PROPERTY_TIMEOUT_FACTORY))
	{
		timeoutHandlerFactory* tof = platformDependant::getHandler()->
			getTimeoutHandlerFactory(GET_PROPERTY(string, PROPERTY_TIMEOUT_FACTORY));

		m_timeoutHandler = tof->create();
	}

	// Create and connect the socket
	socketFactory* sf = platformDependant::getHandler()->
		getSocketFactory(GET_PROPERTY(string, PROPERTY_SERVER_SOCKETFACTORY));

	m_socket = sf->create();
	m_socket->connect(address, port);


	m_tag = vmime::create <IMAPTag>();
	m_parser = vmime::create <IMAPParser>(m_tag, m_socket, m_timeoutHandler);


	setState(STATE_NON_AUTHENTICATED);


	// Connection greeting
	//
	// eg:  C: <connection to server>
	// ---  S: * OK mydomain.org IMAP4rev1 v12.256 server ready

	utility::auto_ptr <IMAPParser::greeting> greet(m_parser->readGreeting());

	if (greet->resp_cond_bye())
	{
		internalDisconnect();
		throw exceptions::connection_greeting_error(m_parser->lastLine());
	}
	else if (greet->resp_cond_auth()->condition() != IMAPParser::resp_cond_auth::PREAUTH)
	{
		try
		{
			authenticate();
		}
		catch (...)
		{
			m_state = STATE_NONE;
			throw;
		}
	}

	// Get the hierarchy separator character
	initHierarchySeparator();

	// Switch to state "Authenticated"
	setState(STATE_AUTHENTICATED);
}


void IMAPConnection::authenticate()
{
	getAuthenticator()->setService(thisRef().dynamicCast <service>());

#if VMIME_HAVE_SASL_SUPPORT
	// First, try SASL authentication
	if (GET_PROPERTY(bool, PROPERTY_OPTIONS_SASL))
	{
		try
		{
			authenticateSASL();
			return;
		}
		catch (exceptions::authentication_error& e)
		{
			if (!GET_PROPERTY(bool, PROPERTY_OPTIONS_SASL_FALLBACK))
			{
				// Can't fallback on normal authentication
				internalDisconnect();
				throw e;
			}
			else
			{
				// Ignore, will try normal authentication
			}
		}
		catch (exception& e)
		{
			internalDisconnect();
			throw e;
		}
	}
#endif // VMIME_HAVE_SASL_SUPPORT

	// Normal authentication
	const string username = getAuthenticator()->getUsername();
	const string password = getAuthenticator()->getPassword();

	send(true, "LOGIN " + IMAPUtils::quoteString(username)
		+ " " + IMAPUtils::quoteString(password), true);

	utility::auto_ptr <IMAPParser::response> resp(m_parser->readResponse());

	if (resp->isBad())
	{
		internalDisconnect();
		throw exceptions::command_error("LOGIN", m_parser->lastLine());
	}
	else if (resp->response_done()->response_tagged()->
			resp_cond_state()->status() != IMAPParser::resp_cond_state::OK)
	{
		internalDisconnect();
		throw exceptions::authentication_error(m_parser->lastLine());
	}
}


#if VMIME_HAVE_SASL_SUPPORT

void IMAPConnection::authenticateSASL()
{
	if (!getAuthenticator().dynamicCast <security::sasl::SASLAuthenticator>())
		throw exceptions::authentication_error("No SASL authenticator available.");

	const std::vector <string> capa = getCapabilities();
	std::vector <string> saslMechs;

	for (unsigned int i = 0 ; i < capa.size() ; ++i)
	{
		const string& x = capa[i];

		if (x.length() > 5 &&
		    (x[0] == 'A' || x[0] == 'a') &&
		    (x[1] == 'U' || x[1] == 'u') &&
		    (x[2] == 'T' || x[2] == 't') &&
		    (x[3] == 'H' || x[3] == 'h') &&
		    x[4] == '=')
		{
			saslMechs.push_back(string(x.begin() + 5, x.end()));
		}
	}

	if (saslMechs.empty())
		throw exceptions::authentication_error("No SASL mechanism available.");

	std::vector <ref <security::sasl::SASLMechanism> > mechList;

	ref <security::sasl::SASLContext> saslContext =
		vmime::create <security::sasl::SASLContext>();

	for (unsigned int i = 0 ; i < saslMechs.size() ; ++i)
	{
		try
		{
			mechList.push_back
				(saslContext->createMechanism(saslMechs[i]));
		}
		catch (exceptions::no_such_mechanism&)
		{
			// Ignore mechanism
		}
	}

	if (mechList.empty())
		throw exceptions::authentication_error("No SASL mechanism available.");

	// Try to suggest a mechanism among all those supported
	ref <security::sasl::SASLMechanism> suggestedMech =
		saslContext->suggestMechanism(mechList);

	if (!suggestedMech)
		throw exceptions::authentication_error("Unable to suggest SASL mechanism.");

	// Allow application to choose which mechanisms to use
	mechList = getAuthenticator().dynamicCast <security::sasl::SASLAuthenticator>()->
		getAcceptableMechanisms(mechList, suggestedMech);

	if (mechList.empty())
		throw exceptions::authentication_error("No SASL mechanism available.");

	// Try each mechanism in the list in turn
	for (unsigned int i = 0 ; i < mechList.size() ; ++i)
	{
		ref <security::sasl::SASLMechanism> mech = mechList[i];

		ref <security::sasl::SASLSession> saslSession =
			saslContext->createSession("imap", getAuthenticator(), mech);

		saslSession->init();

		send(true, "AUTHENTICATE " + mech->getName(), true);

		for (bool cont = true ; cont ; )
		{
			utility::auto_ptr <IMAPParser::response> resp(m_parser->readResponse());

			if (resp->response_done() &&
			    resp->response_done()->response_tagged() &&
			    resp->response_done()->response_tagged()->resp_cond_state()->
			    	status() == IMAPParser::resp_cond_state::OK)
			{
				m_socket = saslSession->getSecuredSocket(m_socket);
				return;
			}
			else
			{
				std::vector <IMAPParser::continue_req_or_response_data*>
					respDataList = resp->continue_req_or_response_data();

				string response;

				for (unsigned int i = 0 ; i < respDataList.size() ; ++i)
				{
					if (respDataList[i]->continue_req())
					{
						response = respDataList[i]->continue_req()->resp_text()->text();
						break;
					}
				}

				if (response.empty())
				{
					cont = false;
					continue;
				}

				byte* challenge = 0;
				int challengeLen = 0;

				byte* resp = 0;
				int respLen = 0;

				try
				{
					// Extract challenge
					saslContext->decodeB64(response, &challenge, &challengeLen);

					// Prepare response
					saslSession->evaluateChallenge
						(challenge, challengeLen, &resp, &respLen);

					// Send response
					send(false, saslContext->encodeB64(resp, respLen), true);
				}
				catch (exceptions::sasl_exception& e)
				{
					if (challenge)
					{
						delete [] challenge;
						challenge = NULL;
					}

					if (resp)
					{
						delete [] resp;
						resp = NULL;
					}

					// Cancel SASL exchange
					send(false, "*", true);
				}
				catch (...)
				{
					if (challenge)
						delete [] challenge;

					if (resp)
						delete [] resp;

					throw;
				}

				if (challenge)
					delete [] challenge;

				if (resp)
					delete [] resp;
			}
		}
	}

	throw exceptions::authentication_error
		("Could not authenticate using SASL: all mechanisms failed.");
}

#endif // VMIME_HAVE_SASL_SUPPORT


const std::vector <string> IMAPConnection::getCapabilities()
{
	send(true, "CAPABILITY", true);

	utility::auto_ptr <IMAPParser::response> resp(m_parser->readResponse());

	std::vector <string> res;

	if (resp->response_done()->response_tagged()->
		resp_cond_state()->status() == IMAPParser::resp_cond_state::OK)
	{
		const std::vector <IMAPParser::continue_req_or_response_data*>& respDataList =
			resp->continue_req_or_response_data();

		for (unsigned int i = 0 ; i < respDataList.size() ; ++i)
		{
			if (respDataList[i]->response_data() == NULL)
				continue;

			const IMAPParser::capability_data* capaData =
				respDataList[i]->response_data()->capability_data();

			std::vector <IMAPParser::capability*> caps = capaData->capabilities();

			for (unsigned int j = 0 ; j < caps.size() ; ++j)
			{
				if (caps[j]->auth_type())
					res.push_back("AUTH=" + caps[j]->auth_type()->name());
				else
					res.push_back(caps[j]->atom()->value());
			}
		}
	}

	return res;
}


ref <security::authenticator> IMAPConnection::getAuthenticator()
{
	return m_auth;
}


const bool IMAPConnection::isConnected() const
{
	return (m_socket && m_socket->isConnected() &&
	        (m_state == STATE_AUTHENTICATED || m_state == STATE_SELECTED));
}


void IMAPConnection::disconnect()
{
	if (!isConnected())
		throw exceptions::not_connected();

	internalDisconnect();
}


void IMAPConnection::internalDisconnect()
{
	if (isConnected())
	{
		send(true, "LOGOUT", true);

		m_socket->disconnect();
		m_socket = NULL;
	}

	m_timeoutHandler = NULL;

	m_state = STATE_LOGOUT;
}


void IMAPConnection::initHierarchySeparator()
{
	send(true, "LIST \"\" \"\"", true);

	vmime::utility::auto_ptr <IMAPParser::response> resp(m_parser->readResponse());

	if (resp->isBad() || resp->response_done()->response_tagged()->
		resp_cond_state()->status() != IMAPParser::resp_cond_state::OK)
	{
		internalDisconnect();
		throw exceptions::command_error("LIST", m_parser->lastLine(), "bad response");
	}

	const std::vector <IMAPParser::continue_req_or_response_data*>& respDataList =
		resp->continue_req_or_response_data();

	if (respDataList.size() < 1 || respDataList[0]->response_data() == NULL)
	{
		internalDisconnect();
		throw exceptions::command_error("LIST", m_parser->lastLine(), "unexpected response");
	}

	const IMAPParser::mailbox_data* mailboxData =
		static_cast <const IMAPParser::response_data*>(respDataList[0]->response_data())->
			mailbox_data();

	if (mailboxData == NULL || mailboxData->type() != IMAPParser::mailbox_data::LIST)
	{
		internalDisconnect();
		throw exceptions::command_error("LIST", m_parser->lastLine(), "invalid type");
	}

	if (mailboxData->mailbox_list()->quoted_char() == '\0')
	{
		internalDisconnect();
		throw exceptions::command_error("LIST", m_parser->lastLine(), "no hierarchy separator");
	}

	m_hierarchySeparator = mailboxData->mailbox_list()->quoted_char();
}


void IMAPConnection::send(bool tag, const string& what, bool end)
{
#if VMIME_DEBUG
	std::ostringstream oss;

	if (tag)
	{
		++(*m_tag);

		oss << string(*m_tag);
		oss << " ";
	}

	oss << what;

	if (end)
		oss << "\r\n";

	m_socket->send(oss.str());
#else
	if (tag)
	{
		++(*m_tag);

		m_socket->send(*m_tag);
		m_socket->send(" ");
	}

	m_socket->send(what);

	if (end)
	{
		m_socket->send("\r\n");
	}
#endif
}


void IMAPConnection::sendRaw(const char* buffer, const int count)
{
	m_socket->sendRaw(buffer, count);
}


IMAPParser::response* IMAPConnection::readResponse(IMAPParser::literalHandler* lh)
{
	return (m_parser->readResponse(lh));
}


const IMAPConnection::ProtocolStates IMAPConnection::state() const
{
	return (m_state);
}


void IMAPConnection::setState(const ProtocolStates state)
{
	m_state = state;
}

const char IMAPConnection::hierarchySeparator() const
{
	return (m_hierarchySeparator);
}


ref <const IMAPTag> IMAPConnection::getTag() const
{
	return (m_tag);
}


ref <const IMAPParser> IMAPConnection::getParser() const
{
	return (m_parser);
}


weak_ref <const IMAPStore> IMAPConnection::getStore() const
{
	return (m_store);
}


weak_ref <IMAPStore> IMAPConnection::getStore()
{
	return (m_store);
}


ref <session> IMAPConnection::getSession()
{
	return (m_store->getSession());
}


} // imap
} // net
} // vmime
