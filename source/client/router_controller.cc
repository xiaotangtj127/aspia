//
// Aspia Project
// Copyright (C) 2016-2025 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#include "client/router_controller.h"

#include "base/logging.h"
#include "base/task_runner.h"
#include "base/peer/client_authenticator.h"
#include "proto/router_peer.pb.h"

namespace client {

//--------------------------------------------------------------------------------------------------
RouterController::RouterController(const RouterConfig& router_config,
                                   std::shared_ptr<base::TaskRunner> task_runner)
    : task_runner_(std::move(task_runner)),
      router_config_(router_config)
{
    LOG(LS_INFO) << "Ctor";
    DCHECK(task_runner_);
}

//--------------------------------------------------------------------------------------------------
RouterController::~RouterController()
{
    LOG(LS_INFO) << "Dtor";
}

//--------------------------------------------------------------------------------------------------
void RouterController::connectTo(base::HostId host_id, bool wait_for_host, Delegate* delegate)
{
    host_id_ = host_id;
    wait_for_host_ = wait_for_host;
    delegate_ = delegate;

    DCHECK_NE(host_id_, base::kInvalidHostId);
    DCHECK(delegate_);

    LOG(LS_INFO) << "Connecting to router... (host_id=" << host_id_ << " wait_for_host="
                 << wait_for_host_ << ")";

    channel_ = std::make_unique<base::TcpChannel>();
    channel_->setListener(this);
    channel_->connect(router_config_.address, router_config_.port);
}

//--------------------------------------------------------------------------------------------------
void RouterController::onTcpConnected()
{
    LOG(LS_INFO) << "Connection to the router is established";

    channel_->setKeepAlive(true);
    channel_->setNoDelay(true);

    authenticator_ = std::make_unique<base::ClientAuthenticator>(task_runner_);

    authenticator_->setIdentify(proto::IDENTIFY_SRP);
    authenticator_->setUserName(router_config_.username);
    authenticator_->setPassword(router_config_.password);
    authenticator_->setSessionType(proto::ROUTER_SESSION_CLIENT);

    authenticator_->start(std::move(channel_),
                          [this](base::ClientAuthenticator::ErrorCode error_code)
    {
        if (error_code == base::ClientAuthenticator::ErrorCode::SUCCESS)
        {
            const base::Version& router_version = authenticator_->peerVersion();

            if (delegate_)
                delegate_->onRouterConnected(router_version);
            else
                LOG(LS_ERROR) << "Invalid delegate";

            // The authenticator takes the listener on itself, we return the receipt of
            // notifications.
            channel_ = authenticator_->takeChannel();
            channel_->setListener(this);

            if (router_version >= base::Version::kVersion_2_6_0)
            {
                LOG(LS_INFO) << "Using channel id support";
                channel_->setChannelIdSupport(true);
            }

            LOG(LS_INFO) << "Sending connection request (host_id: " << host_id_ << ")";

            // Now the session will receive incoming messages.
            channel_->resume();

            sendConnectionRequest();
        }
        else
        {
            LOG(LS_ERROR) << "Authentication failed: "
                          << base::ClientAuthenticator::errorToString(error_code);

            if (delegate_)
            {
                Error error;
                error.type = ErrorType::AUTHENTICATION;
                error.code.authentication = error_code;

                delegate_->onErrorOccurred(error);
            }
            else
            {
                LOG(LS_ERROR) << "Invalid delegate";
            }
        }

        // Authenticator is no longer needed.
        task_runner_->deleteSoon(std::move(authenticator_));
    });
}

//--------------------------------------------------------------------------------------------------
void RouterController::onTcpDisconnected(base::NetworkChannel::ErrorCode error_code)
{
    LOG(LS_INFO) << "Connection to the router is lost ("
                 << base::TcpChannel::errorToString(error_code) << ")";

    if (!delegate_)
    {
        LOG(LS_ERROR) << "Invalid delegate";
        return;
    }

    Error error;
    error.type = ErrorType::NETWORK;
    error.code.network = error_code;

    delegate_->onErrorOccurred(error);
}

//--------------------------------------------------------------------------------------------------
void RouterController::onTcpMessageReceived(uint8_t /* channel_id */, const base::ByteArray& buffer)
{
    Error error;
    error.type = ErrorType::ROUTER;

    proto::RouterToPeer message;
    if (!base::parse(buffer, &message))
    {
        LOG(LS_ERROR) << "Invalid message from router";

        error.code.router = ErrorCode::UNKNOWN_ERROR;
        if (delegate_)
            delegate_->onErrorOccurred(error);
        else
            LOG(LS_ERROR) << "Invalid delegate";
        return;
    }

    if (message.has_connection_offer())
    {
        if (relay_peer_)
        {
            LOG(LS_ERROR) << "Re-offer connection detected";
            return;
        }

        const proto::ConnectionOffer& connection_offer = message.connection_offer();

        if (connection_offer.error_code() != proto::ConnectionOffer::SUCCESS ||
            connection_offer.peer_role() != proto::ConnectionOffer::CLIENT)
        {
            if (delegate_)
            {
                switch (connection_offer.error_code())
                {
                    case proto::ConnectionOffer::PEER_NOT_FOUND:
                        error.code.router = ErrorCode::PEER_NOT_FOUND;
                        break;

                    case proto::ConnectionOffer::ACCESS_DENIED:
                        error.code.router = ErrorCode::ACCESS_DENIED;
                        break;

                    case proto::ConnectionOffer::KEY_POOL_EMPTY:
                        error.code.router = ErrorCode::KEY_POOL_EMPTY;
                        break;

                    default:
                        error.code.router = ErrorCode::UNKNOWN_ERROR;
                        break;
                }

                if (error.code.router == ErrorCode::PEER_NOT_FOUND && wait_for_host_)
                {
                    LOG(LS_INFO) << "Host is OFFLINE. Wait for host";
                    waitForHost();
                }
                else
                {
                    delegate_->onErrorOccurred(error);
                }
            }
            else
            {
                LOG(LS_ERROR) << "Invalid delegate";
            }
        }
        else
        {
            relay_peer_ = std::make_unique<base::RelayPeer>();
            relay_peer_->start(connection_offer, this);
        }
    }
    else if (message.has_host_status())
    {
        const proto::HostStatus& host_status = message.host_status();
        if (host_status.status() == proto::HostStatus::STATUS_ONLINE)
        {
            LOG(LS_INFO) << "Host is ONLINE now. Sending connection request";
            sendConnectionRequest();
        }
        else
        {
            LOG(LS_INFO) << "Host is OFFLINE. Wait for host";
            waitForHost();
        }
    }
    else
    {
        LOG(LS_ERROR) << "Unhandled message from router";
    }
}

//--------------------------------------------------------------------------------------------------
void RouterController::onTcpMessageWritten(
    uint8_t /* channel_id */, base::ByteArray&& /* buffer */, size_t /* pending */)
{
    // Nothing
}

//--------------------------------------------------------------------------------------------------
void RouterController::onRelayConnectionReady(std::unique_ptr<base::TcpChannel> channel)
{
    LOG(LS_INFO) << "Relay connection ready";

    if (delegate_)
        delegate_->onHostConnected(std::move(channel));
    else
        LOG(LS_ERROR) << "Invalid delegate";
}

//--------------------------------------------------------------------------------------------------
void RouterController::onRelayConnectionError()
{
    LOG(LS_INFO) << "Relay connection error";

    if (!delegate_)
    {
        LOG(LS_ERROR) << "Invalid delegate";
        return;
    }

    Error error;
    error.type = ErrorType::ROUTER;
    error.code.router = ErrorCode::RELAY_ERROR;

    delegate_->onErrorOccurred(error);
}

//--------------------------------------------------------------------------------------------------
void RouterController::sendConnectionRequest()
{
    LOG(LS_INFO) << "Sending connection request (host_id=" << host_id_ << ")";

    proto::PeerToRouter message;
    message.mutable_connection_request()->set_host_id(host_id_);
    channel_->send(proto::ROUTER_CHANNEL_ID_SESSION, base::serialize(message));
}

//--------------------------------------------------------------------------------------------------
void RouterController::waitForHost()
{
    LOG(LS_INFO) << "Wait for host";

    status_request_timer_ = std::make_unique<base::WaitableTimer>(
        base::WaitableTimer::Type::SINGLE_SHOT, task_runner_);

    if (delegate_)
        delegate_->onHostAwaiting();
    else
        LOG(LS_ERROR) << "Invalid delegate";

    status_request_timer_->start(std::chrono::milliseconds(5000), [this]()
    {
        LOG(LS_INFO) << "Request host status from router (host_id=" << host_id_ << ")";

        proto::PeerToRouter message;
        message.mutable_check_host_status()->set_host_id(host_id_);
        channel_->send(proto::ROUTER_CHANNEL_ID_SESSION, base::serialize(message));
    });
}

} // namespace client
