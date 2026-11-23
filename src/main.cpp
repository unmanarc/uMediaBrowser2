#include <Mantids30/Network/Sockets/socket_tcp.h>
#include <Mantids30/Runtime/Logs/applog.h>
#include <Mantids30/Runtime/Service/application.h>
#ifdef WITH_SSL_SUPPORT
#include <Mantids30/Network/Sockets/socket_tls.h>
#endif
#include <Mantids30/Core/Memory/a_bool.h>
#include <Mantids30/Core/Memory/a_uint16.h>
#include <Mantids30/Network/Sockets/acceptor_multithreaded.h>

#include <charconv>
#include <cstdlib>
#include <limits>
#include <filesystem>
#include <fstream>
#include <inttypes.h>
#include <sys/time.h>

#include "config.h"
#include "appconfig.h"
#include "favicon.h"
#include "html_errors.h"
#include "webclienthdlr.h"

using namespace Mantids30;
using namespace Mantids30::Runtime;
using namespace Mantids30::Core::Memory;
using namespace Mantids30::Core::Memory::Abstract;
using namespace Mantids30::Runtime;
using namespace Mantids30::Runtime::Logs;
using namespace Mantids30::Runtime::Service;
namespace fs = std::filesystem;

class UMediaBrowser : public Mantids30::Runtime::Service::Application
{
public:
    static std::shared_ptr<Mantids30::Core::Memory::Containers::B_MEM> favicon;
    static time_t staticContentTime;

    explicit UMediaBrowser(boost::property_tree::ptree configuration) : configuration(std::move(configuration))
    {
        multiThreadedAcceptor = std::make_shared<Mantids30::Network::Sockets::Acceptors::MultiThreaded>();
    }

    void _shutdown() override { log->log0(__func__, Logs::INFO, "SIGTERM received."); }

    void _initvars(int argc, char *argv[], Arguments::GlobalArguments *globalArguments) override
    {
        globalArguments->setInifiniteWaitAtEnd(true);

        favicon = std::make_shared<Mantids30::Core::Memory::Containers::B_MEM>(favicon2_ico, favicon2_ico_len);
        staticContentTime = time(nullptr);

        /////////////////////////
        globalArguments->setVersion(std::stoi(PROJECT_VER_MAJOR), std::stoi(PROJECT_VER_MINOR), std::stoi(PROJECT_VER_PATCH), "a");

        webClientParameters.softwareVersion = globalArguments->getVersion();
        globalArguments->softwareLicense = "AGPL";
        globalArguments->softwareDescription = std::string(PROJECT_DESCRIPTION);
        globalArguments->addAuthor({"Aarón Mizrachi", "aaron@unmanarc.com"});
    }

    bool _config(int argc, char *argv[], Arguments::GlobalArguments *globalArguments) override
    {
#ifdef WITH_SSL_SUPPORT
        Mantids30::Network::Sockets::Socket_TLS::prepareTLS();
#endif
        bool configUseFancy = !configuration.get<bool>("logging.sys");
        const bool debugMode = configuration.get<bool>("logging.debug");

        log = new Logs::AppLog();
        log->enableEmptyFieldLogging = true;
        log->userFieldMinWidth = 1;
        log->enableAttributeNameLogging = false;
        log->enableColorLogging = configUseFancy;
        log->enableDateLogging = configUseFancy;
        log->moduleFieldMinWidth = 36;
        log->setDebug(debugMode);

        webClientParameters.rpcLog = new Logs::RPCLog();
        webClientParameters.rpcLog->enableEmptyFieldLogging = false;
        webClientParameters.rpcLog->userFieldMinWidth = 1;
        webClientParameters.rpcLog->enableAttributeNameLogging = false;
        webClientParameters.rpcLog->enableColorLogging = configUseFancy;
        webClientParameters.rpcLog->enableDateLogging = configUseFancy;
        webClientParameters.rpcLog->moduleFieldMinWidth = 36;
        webClientParameters.rpcLog->setDebug(debugMode);

        std::string passFile = configuration.get<std::string>("security.passfile");

        if (const char *apiKey = std::getenv("APIKEY"))
            webClientParameters.apiKey = apiKey;

        if (!passFile.empty())
        {
            std::ifstream file(passFile);
            if (file.is_open())
            {
                if (!std::getline(file, webClientParameters.user))
                {
                    log->log0(__func__, Logs::CRITICAL, "Password File '%s' require at least 2 lines [user/pass)", passFile.c_str());
                    exit(-14);
                }
                if (!std::getline(file, webClientParameters.pass))
                {
                    log->log0(__func__, Logs::CRITICAL, "Password File '%s' require at least 2 lines (user/pass]", passFile.c_str());
                    exit(-15);
                }
                file.close();
            }
            else
            {
                log->log0(__func__, Logs::CRITICAL, "Failed to open user/password file '%s'", passFile.c_str());
                exit(-13);
            }
        }

        listenAddress = configuration.get<std::string>("http.laddr");
        webClientParameters.httpDocumentRootDir = configuration.get<std::string>("http.rootdir");
        listenPort = static_cast<uint16_t>(std::stoul(configuration.get<std::string>("http.lport")));
        webClientParameters.canArchive = configuration.get<bool>("permissions.can-archive");
        webClientParameters.canUpload = configuration.get<bool>("permissions.can-upload");
        webClientParameters.canOverwrite = configuration.get<bool>("permissions.can-overwrite");
        webClientParameters.debug = configuration.get<bool>("logging.debug");
        webClientParameters.useXRealIP = configuration.get<bool>("http.use-x-real-ip");
        webClientParameters.appTitle = configuration.get<std::string>("http.title");

        const auto prefilterFile = configuration.get<std::string>("security.prefilter");
        if (!prefilterFile.empty())
        {
            try
            {
                std::ifstream input(prefilterFile, std::ios::binary);
                if (!input) throw std::runtime_error("Cannot open file");
                // Read one byte past the limit to reject oversized files without loading them.
                std::string json(65537, '\0');
                input.read(json.data(), json.size());
                json.resize(static_cast<size_t>(input.gcount()));
                if (input.bad()) throw std::runtime_error("Cannot read file");
                prefilter = TimelineFilter::parsePrefilter(json);
                log->log0(__func__, Logs::INFO, "Loaded mandatory prefilter from %s", prefilterFile.c_str());
            }
            catch (const std::exception &error)
            {
                log->log0(__func__, Logs::CRITICAL, "Invalid prefilter '%s': %s", prefilterFile.c_str(), error.what());
                return false;
            }
        }

        // Parse explicitly so zero, negatives, trailing text and overflow fail
        // before opening the listener. Conversion to seconds must also be safe.
        const auto gapText = configuration.get<std::string>("timeline.timeline-gap-minutes");
        int64_t gapMinutes = 0;
        const auto parsedGap = std::from_chars(gapText.data(), gapText.data() + gapText.size(), gapMinutes);
        if (parsedGap.ec != std::errc{} || parsedGap.ptr != gapText.data() + gapText.size() ||
            gapMinutes <= 0 || gapMinutes > std::numeric_limits<int64_t>::max() / 60)
        {
            log->log0(__func__, Logs::CRITICAL, "Invalid timeline-gap-minutes: expected a positive integer number of minutes.");
            return false;
        }
        timelineGapSeconds = gapMinutes * 60;

        const auto excludeFile = configuration.get<std::string>("timeline.tag-exclude-file");
        if (!excludeFile.empty())
        {
            std::ifstream input(excludeFile);
            if (!input)
            {
                log->log0(__func__, Logs::CRITICAL, "Cannot read tag-exclude-file: %s", excludeFile.c_str());
                return false;
            }
            std::string line;
            size_t lineNumber = 0;
            while (std::getline(input, line))
            {
                ++lineNumber;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto first = line.find_first_not_of(" \t\r\n");
                if (first == std::string::npos || line[first] == '#') continue;
                try { tagExclusions.emplace_back(line, std::regex::ECMAScript | std::regex::optimize); }
                catch (const std::regex_error &error)
                {
                    log->log0(__func__, Logs::CRITICAL, "Invalid tag-exclude-file regex at %s:%zu: %s", excludeFile.c_str(), lineNumber, error.what());
                    return false;
                }
            }
            if (!input.eof())
            {
                log->log0(__func__, Logs::CRITICAL, "Cannot read tag-exclude-file: %s", excludeFile.c_str());
                return false;
            }
            log->log0(__func__, Logs::INFO, "Loaded %zu tag exclusion rules from %s", tagExclusions.size(), excludeFile.c_str());
        }

        bool configUseIPv4 = configuration.get<bool>("http.ipv4");
        auto configThreads = static_cast<uint32_t>(std::stoul(configuration.get<std::string>("http.threads")));

        auto socketTCP = std::make_shared<Mantids30::Network::Sockets::Socket_TCP>();
        std::shared_ptr<Mantids30::Network::Sockets::Socket_Stream> acceptorSocket = socketTCP;
#ifdef WITH_SSL_SUPPORT
        if (configuration.get<bool>("tls.enabled"))
        {
            const auto keyfile = configuration.get<std::string>("tls.keyfile");
            const auto certfile = configuration.get<std::string>("tls.certfile");
            const auto cafile = configuration.get<std::string>("tls.cafile");
            auto socketTLS = std::make_shared<Mantids30::Network::Sockets::Socket_TLS>();
            // Use the system's default TLS security level.
            socketTLS->tlsKeys.setSecurityLevel(-1);
            if (!socketTLS->tlsKeys.loadPrivateKeyFromPEMFile(keyfile.c_str()))
            {
                log->log0(__func__, Logs::CRITICAL, "Cannot load tls.keyfile: %s", keyfile.c_str());
                return false;
            }
            if (!socketTLS->tlsKeys.loadPublicKeyFromPEMFile(certfile.c_str()))
            {
                log->log0(__func__, Logs::CRITICAL, "Cannot load tls.certfile: %s", certfile.c_str());
                return false;
            }
            if (!cafile.empty() && !socketTLS->tlsKeys.loadCAFromPEMFile(cafile))
            {
                log->log0(__func__, Logs::CRITICAL, "Cannot load tls.cafile: %s", cafile.c_str());
                return false;
            }
            acceptorSocket = socketTLS;
        }
#endif
        acceptorSocket->setUseIPv6(!configUseIPv4);
        if (!acceptorSocket->listenOn(listenPort, listenAddress.c_str()))
        {
            log->log0(__func__, Logs::CRITICAL, "Unable to listen at %s:%" PRIu16, listenAddress.c_str(), listenPort);
            exit(-20);
            return false;
        }
        multiThreadedAcceptor->addAcceptorSocket(acceptorSocket);
        multiThreadedAcceptor->callbacks.setAllContexts(this);
        multiThreadedAcceptor->callbacks.onClientConnected = _callbackOnConnect;
        multiThreadedAcceptor->callbacks.onProtocolInitializationFailure = _callbackOnInitFailed;
        multiThreadedAcceptor->callbacks.onClientAcceptTimeoutOccurred = _callbackOnTimeOut;
        multiThreadedAcceptor->parameters.setMaxConcurrentClients(configThreads);

        return true;
    }

    int _start(int argc, char *argv[], Arguments::GlobalArguments *globalArguments) override
    {
        std::error_code ec;
        const fs::path rp = fs::canonical(webClientParameters.httpDocumentRootDir, ec);

        if (ec)
        {
            log->log0(__func__, Logs::CRITICAL, "Invalid Document Root Directory.");
            exit(-1);
        }
        webClientParameters.httpDocumentRootDir = rp.string();
        // Scan existing paths once for the tag picker; digiKam queries remain request-local.
        webClientParameters.timeline = std::make_shared<TimelineDatabase>(rp.string(), timelineGapSeconds, log, std::move(tagExclusions), std::move(prefilter));
        log->log0(__func__, Logs::INFO, "Timeline uses read-only digiKam queries; tag selector file snapshot loaded.");
        multiThreadedAcceptor->startInBackground();

        log->log0(__func__, Logs::INFO, "Web Server Loaded @%s:%" PRIu16, listenAddress.c_str(), listenPort);
        log->log0(__func__, Logs::INFO, "Document root: %s", rp.c_str());
        log->log0(__func__, Logs::INFO, "Features: can-upload=%d can-overwrite=%d can-archive=%d",
                  webClientParameters.canUpload,
                  webClientParameters.canOverwrite,
                  webClientParameters.canArchive);
        log->log0(__func__, Logs::INFO, "Auth: %s | Debug logging: %s",
                  webClientParameters.pass.empty() ? "disabled" : "basic",
                  webClientParameters.debug ? "enabled" : "disabled");
        if (!webClientParameters.apiKey.empty())
            log->log0(__func__, Logs::INFO, "API Key authentication enabled (API-Key header required).");

        return 0;
    }

    const webClientParams &getWebClientParameters() const;

private:
    /**
     * callback when connection is fully established
     */
    static void _callbackOnConnect(void *, const std::shared_ptr<Mantids30::Network::Sockets::Socket_Stream> &);
    /**
     * callback when protocol initialization failed (like bad X.509 on TLS)
     */
    static void _callbackOnInitFailed(void *, const std::shared_ptr<Mantids30::Network::Sockets::Socket_Stream> &);
    /**
     * callback when timed out (all the thread queues are saturated) (this callback is called from acceptor thread, you should use it very quick)
     */
    static void _callbackOnTimeOut(void *, const std::shared_ptr<Mantids30::Network::Sockets::Socket_Stream> &);

    boost::property_tree::ptree configuration;
    std::string listenAddress;
    uint16_t listenPort;
    int64_t timelineGapSeconds = 30 * 60; ///< INFO-configured inactivity threshold in seconds.
    std::vector<std::regex> tagExclusions; ///< Compiled once during configuration.
    std::optional<TimelineFilter> prefilter; ///< Validated before starting the listener.
    Logs::AppLog *log;
    webClientParams webClientParameters;

    std::shared_ptr<Mantids30::Network::Sockets::Acceptors::MultiThreaded> multiThreadedAcceptor;
};

void UMediaBrowser::_callbackOnConnect(void *obj, const std::shared_ptr<Mantids30::Network::Sockets::Socket_Stream> &s)
{
    std::string tlsCN;
    UMediaBrowser *webServer = (UMediaBrowser *) obj;

    bool isSecure = s->isSecure();
    if (isSecure)
    {
#ifdef WITH_SSL_SUPPORT
        Network::Sockets::Socket_TLS *tlsSock = (Network::Sockets::Socket_TLS *) s.get();
        tlsCN = tlsSock->getTLSPeerCN();
#endif
    }

    // Prepare the web services handler.
    auto webHandler = std::make_shared<WebClientHdlr>(s);

    Mantids30::Protocol::HTTP::HTTPv1_Server::StaticContentElement faviconElement;
    faviconElement.setData(favicon, staticContentTime);

    // Mantids30 resolves in-memory static content without the leading slash.
    // Register both variants to keep the original URL mapping working.
    webHandler->addStaticContent("favicon.ico", faviconElement);
    webHandler->addStaticContent("/favicon.ico", faviconElement);

    char inetAddr[INET6_ADDRSTRLEN];
    s->getRemotePair(inetAddr);

    webServer->log->log1("web", inetAddr, Logs::INFO, "C/CONNECT: %s%s%s",
                         inetAddr,
                         isSecure ? " [TLS]" : "",
                         tlsCN.empty() ? "" : (" CN=" + tlsCN).c_str());

    webHandler->clientRequest.networkClientInfo.setClientInformation(inetAddr, isSecure, tlsCN);
    webHandler->setWebClientParameters(webServer->getWebClientParameters());

    // Handle the Web Client here:
    Core::Memory::Streams::Parser::ParseResult err;
    webHandler->parseObject(&err);
}

void UMediaBrowser::_callbackOnInitFailed(void *obj, const std::shared_ptr<Mantids30::Network::Sockets::Socket_Stream> &s)
{
    char inetAddr[INET6_ADDRSTRLEN];
    s->getRemotePair(inetAddr);
    ((UMediaBrowser *) obj)->log->log1("web", inetAddr, Logs::WARNING, "C/INIT-FAIL: protocol initialization failed (eg. bad TLS handshake)");
}

void UMediaBrowser::_callbackOnTimeOut(void *obj, const std::shared_ptr<Mantids30::Network::Sockets::Socket_Stream> &s)
{
    char inetAddr[INET6_ADDRSTRLEN];
    s->getRemotePair(inetAddr);
    ((UMediaBrowser *) obj)->log->log1("web", inetAddr, Logs::WARNING, "C/TIMEOUT: no request received in time, all thread queues saturated, sending 503");
    s->writeString(HTML_503_RESPONSE);
}

const webClientParams &UMediaBrowser::getWebClientParameters() const
{
    return webClientParameters;
}

int main(int argc, char *argv[])
{
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h"))
    {
        fprintf(stdout, "Usage: %s <configuration.info>\nConfiguration format: Boost.PropertyTree INFO. See uMediaBrowser2.info.example.\n", argv[0]);
        return 0;
    }
    if (argc != 2 || argv[1][0] == '-')
    {
        fprintf(stderr, "Usage: %s <configuration.info>\n", argv[0]);
        return 2;
    }
    try
    {
        auto *application = new UMediaBrowser(readConfiguration(argv[1]));
        // The service runtime must not parse application settings or service flags.
        char *serviceArguments[] = {argv[0], nullptr};
        return StartApplication(1, serviceArguments, application);
    }
    catch (const std::exception &error)
    {
        fprintf(stderr, "Invalid configuration '%s': %s\n", argv[1], error.what());
        return 1;
    }
}

std::shared_ptr<Mantids30::Core::Memory::Containers::B_MEM> UMediaBrowser::favicon = nullptr;
time_t UMediaBrowser::staticContentTime = 0;
