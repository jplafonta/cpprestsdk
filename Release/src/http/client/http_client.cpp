/***
* ==++==
*
* Copyright (c) Microsoft Corporation. All rights reserved.
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* ==--==
* =+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
*
* HTTP Library: Client-side APIs.
*
* This file contains shared code across all http_client implementations.
*
* For the latest on this and related APIs, please see: https://github.com/Microsoft/cpprestsdk
*
* =-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
****/

#include "stdafx.h"

#include "http_client_impl.h"

namespace web { namespace http { namespace client {

// Helper function to check to make sure the uri is valid.
static void verify_uri(const uri &uri)
{
    // Some things like proper URI schema are verified by the URI class.
    // We only need to check certain things specific to HTTP.
    if (uri.scheme() != _XPLATSTR("http") && uri.scheme() != _XPLATSTR("https"))
    {
        throw std::invalid_argument("URI scheme must be 'http' or 'https'");
    }

    if (uri.host().empty())
    {
        throw std::invalid_argument("URI must contain a hostname.");
    }
}

namespace details
{

#if defined(_WIN32)
    extern const utility::char_t * get_with_body_err_msg = _XPLATSTR("A GET or HEAD request should not have an entity body.");
#endif

void request_context::complete_headers()
{
    // We have already read (and transmitted) the request body. Should we explicitly close the stream?
    // Well, there are test cases that assumes that the istream is valid when t receives the response!
    // For now, we will drop our reference which will close the stream if the user doesn't have one.
    m_request.set_body(Concurrency::streams::istream());
    m_request_completion.set(m_response);
}

void request_context::complete_request(utility::size64_t body_size)
{
    m_response._get_impl()->_complete(body_size);

    finish();
}

void request_context::report_error(unsigned long error_code, const std::string &errorMessage)
{
    report_exception(http_exception(static_cast<int>(error_code), errorMessage));
}

#if defined(_WIN32)
void request_context::report_error(unsigned long error_code, const std::wstring &errorMessage)
{
    report_exception(http_exception(static_cast<int>(error_code), errorMessage));
}
#endif

void request_context::report_exception(std::exception_ptr exceptionPtr)
{
    auto response_impl = m_response._get_impl();

    // If cancellation has been triggered then ignore any errors.
    if (m_request._cancellation_token().is_canceled())
    {
        exceptionPtr = std::make_exception_ptr(http_exception((int)std::errc::operation_canceled, std::generic_category()));
    }

    // First try to complete the headers with an exception.
    if (m_request_completion.set_exception(exceptionPtr))
    {
        // Complete the request with no msg body. The exception
        // should only be propagated to one of the tce.
        response_impl->_complete(0);
    }
    else
    {
        // Complete the request with an exception
        response_impl->_complete(0, exceptionPtr);
    }

    finish();
}

concurrency::streams::streambuf<uint8_t> request_context::_get_readbuffer()
{
    auto instream = m_request.body();

    _ASSERTE((bool)instream);
    return instream.streambuf();
}

concurrency::streams::streambuf<uint8_t> request_context::_get_writebuffer()
{
    auto outstream = m_response._get_impl()->outstream();

    _ASSERTE((bool)outstream);
    return outstream.streambuf();
}

request_context::request_context(const std::shared_ptr<_http_client_communicator> &client, const http_request &request)
    : m_http_client(client),
    m_request(request),
    m_uploaded(0),
    m_downloaded(0)
{
    auto responseImpl = m_response._get_impl();

    // Copy the user specified output stream over to the response
    responseImpl->set_outstream(request._get_impl()->_response_stream(), false);

    // Prepare for receiving data from the network. Ideally, this should be done after
    // we receive the headers and determine that there is a response body. We will do it here
    // since it is not immediately apparent where that would be in the callback handler
    responseImpl->_prepare_to_receive_data();
}

void _http_client_communicator::async_send_request(const std::shared_ptr<request_context> &request)
{
    if (m_client_config.guarantee_order())
    {
        // Send to call block to be processed.
        push_request(request);
    }
    else
    {
        // Schedule a task to start sending.
        pplx::create_task([this, request]
        {
            open_and_send_request(request);
        });
    }
}

void _http_client_communicator::finish_request()
{
    // If guarantee order is specified we don't need to do anything.
    if (m_client_config.guarantee_order())
    {
        pplx::extensibility::scoped_critical_section_t l(m_open_lock);

        --m_scheduled;

        if (!m_requests_queue.empty())
        {
            auto request = m_requests_queue.front();
            m_requests_queue.pop();

            // Schedule a task to start sending.
            pplx::create_task([this, request]
            {
                open_and_send_request(request);
            });
        }
    }
}

http_client_config& _http_client_communicator::client_config()
{
    return m_client_config;
}

const uri & _http_client_communicator::base_uri() const
{
    return m_uri;
}

_http_client_communicator::_http_client_communicator(http::uri&& address, http_client_config&& client_config)
    : m_uri(std::move(address)), m_client_config(std::move(client_config)), m_opened(false), m_scheduled(0)
{
}

// Wraps opening the client around sending a request.
void _http_client_communicator::open_and_send_request(const std::shared_ptr<request_context> &request)
{
    // First see if client needs to be opened.
    auto error = open_if_required();

    if (error != 0)
    {
        // Failed to open
        request->report_error(error, _XPLATSTR("Open failed"));

        // DO NOT TOUCH the this pointer after completing the request
        // This object could be freed along with the request as it could
        // be the last reference to this object
        return;
    }

    send_request(request);
}

unsigned long _http_client_communicator::open_if_required()
{
    unsigned long error = 0;

    if (!m_opened)
    {
        pplx::extensibility::scoped_critical_section_t l(m_open_lock);

        // Check again with the lock held
        if (!m_opened)
        {
            error = open();

            if (error == 0)
            {
                m_opened = true;
            }
        }
    }

    return error;
}

void _http_client_communicator::push_request(const std::shared_ptr<request_context> &request)
{
    pplx::extensibility::scoped_critical_section_t l(m_open_lock);

    if (++m_scheduled == 1)
    {
        // Schedule a task to start sending.
        pplx::create_task([this, request]()
        {
            open_and_send_request(request);
        });
    }
    else
    {
        m_requests_queue.push(request);
    }
}

inline void request_context::finish()
{
    // If cancellation is enabled and registration was performed, unregister.
    if (m_cancellationRegistration != pplx::cancellation_token_registration())
    {
        _ASSERTE(m_request._cancellation_token() != pplx::cancellation_token::none());
        m_request._cancellation_token().deregister_callback(m_cancellationRegistration);
    }

    m_http_client->finish_request();
}

} // namespace details

/// <summary>
/// Private implementation of http_client. Manages the http request processing pipeline.
/// </summary>
class http_pipeline
{
public:
    http_pipeline(std::shared_ptr<details::_http_client_communicator> last) : m_last_stage(std::move(last))
    {}

    // pplx::extensibility::recursive_lock_t does not support move/copy, but does not delete the functions either.
    http_pipeline(const http_pipeline &);
    http_pipeline(http_pipeline &&);
    http_pipeline & operator=(const http_pipeline &);
    http_pipeline & operator=(http_pipeline &&);

    /// <summary>
    /// Initiate an http request into the pipeline
    /// </summary>
    /// <param name="request">Http request</param>
    pplx::task<http_response> propagate(http_request request)
    {
        std::shared_ptr<http_pipeline_stage> first;
        {
            pplx::extensibility::scoped_recursive_lock_t l(m_lock);
            first = (m_stages.size() > 0) ? m_stages[0] : m_last_stage;
        }
        return first->propagate(request);
    }

    /// <summary>
    /// Adds an HTTP pipeline stage to the pipeline.
    /// </summary>
    /// <param name="stage">A pipeline stage.</param>
    void append(const std::shared_ptr<http_pipeline_stage> &stage)
    {
        pplx::extensibility::scoped_recursive_lock_t l(m_lock);

        if (m_stages.size() > 0)
        {
            std::shared_ptr<http_pipeline_stage> penultimate = m_stages[m_stages.size() - 1];
            penultimate->set_next_stage(stage);
        }
        stage->set_next_stage(m_last_stage);

        m_stages.push_back(stage);
    }

    // The last stage is always set up by the client or listener and cannot
    // be changed. All application-defined stages are executed before the
    // last stage, which is typically a send or dispatch.
    const std::shared_ptr<details::_http_client_communicator> m_last_stage;

private:

    // The vector of pipeline stages.
    std::vector<std::shared_ptr<http_pipeline_stage>> m_stages;

    pplx::extensibility::recursive_lock_t m_lock;
};

void http_client::add_handler(const std::function<pplx::task<http_response> __cdecl(http_request, std::shared_ptr<http::http_pipeline_stage>)> &handler)
{
    class function_pipeline_wrapper : public http::http_pipeline_stage
    {
    public:
        function_pipeline_wrapper(const std::function<pplx::task<http_response> __cdecl(http_request, std::shared_ptr<http::http_pipeline_stage>)> &handler) : m_handler(handler)
        {
        }

        virtual pplx::task<http_response> propagate(http_request request) override
        {
            return m_handler(std::move(request), next_stage());
        }
    private:

        std::function<pplx::task<http_response>(http_request, std::shared_ptr<http::http_pipeline_stage>)> m_handler;
    };

    m_pipeline->append(std::make_shared<function_pipeline_wrapper>(handler));
}

void http_client::add_handler(const std::shared_ptr<http::http_pipeline_stage> &stage)
{
    m_pipeline->append(stage);
}

http_client::http_client(const uri &base_uri)
{
	init(base_uri, http_client_config());
}

http_client::http_client(const uri &base_uri, const http_client_config &client_config)
{
    init(base_uri, client_config);
}

http_client::~http_client() CPPREST_NOEXCEPT {}

void http_client::init(const uri &base_uri, const http_client_config &client_config)
{
	std::shared_ptr<details::_http_client_communicator> final_pipeline_stage;

    if (base_uri.scheme().empty())
    {
        auto uribuilder = uri_builder(base_uri);
        uribuilder.set_scheme(_XPLATSTR("http"));
        uri uriWithScheme = uribuilder.to_uri();
        verify_uri(uriWithScheme);
        final_pipeline_stage = details::create_platform_final_pipeline_stage(std::move(uriWithScheme), http_client_config(client_config));
    }
    else
    {
        verify_uri(base_uri);
        final_pipeline_stage = details::create_platform_final_pipeline_stage(uri(base_uri), http_client_config(client_config));
    }

    m_pipeline = std::make_shared<http_pipeline>(std::move(final_pipeline_stage));

#if !TV_API
#if !defined(CPPREST_TARGET_XP)
    add_handler(std::static_pointer_cast<http::http_pipeline_stage>(
        std::make_shared<oauth1::details::oauth1_handler>(client_config.oauth1())));
#endif
#endif

    add_handler(std::static_pointer_cast<http::http_pipeline_stage>(
        std::make_shared<oauth2::details::oauth2_handler>(client_config.oauth2())));
}

const http_client_config & http_client::client_config() const
{
    return m_pipeline->m_last_stage->client_config();
}

const uri & http_client::base_uri() const
{
    return m_pipeline->m_last_stage->base_uri();
}

// Macros to help build string at compile time and avoid overhead.
#define STRINGIFY(x) _XPLATSTR(#x)
#define TOSTRING(x) STRINGIFY(x)
#define USERAGENT _XPLATSTR("cpprestsdk/") TOSTRING(CPPREST_VERSION_MAJOR) _XPLATSTR(".") TOSTRING(CPPREST_VERSION_MINOR) _XPLATSTR(".") TOSTRING(CPPREST_VERSION_REVISION)

pplx::task<http_response> http_client::request(http_request request, const pplx::cancellation_token &token)
{
    if (!request.headers().has(header_names::user_agent))
    {
        request.headers().add(header_names::user_agent, USERAGENT);
    }

    request._set_base_uri(base_uri());
    request._set_cancellation_token(token);
    return m_pipeline->propagate(request);
}


}}}