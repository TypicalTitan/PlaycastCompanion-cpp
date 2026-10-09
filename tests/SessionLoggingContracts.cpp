#include "pch.h"
#include "Config.h"
#include "SessionLogging.h"
#include "SessionLoggingClassifier.h"
#include "SessionLoggingInventory.h"
#include "SessionLoggingPipe.h"
#include "SessionLoggingRedactor.h"
#include "SessionLoggingVdf.h"
#include "SessionLoggingWriter.h"
#include <fstream>
#include <iostream>
#include <set>

using namespace pc;
using namespace pc::sessionlog;
namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Fixture {
    std::filesystem::path root;
    Fixture() {
        wchar_t temp[MAX_PATH]{};
        Require(GetTempPathW(MAX_PATH, temp) != 0, "Temporary directory unavailable");
        static std::atomic_uint counter{0};
        root = std::filesystem::absolute(std::filesystem::path(temp) / std::format(L"pc-native-contract-{}-{}-{}", GetCurrentProcessId(), GetTickCount64(), ++counter));
        std::filesystem::create_directories(root);
    }
    ~Fixture() {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        auto parent = std::filesystem::absolute(std::filesystem::path(temp)).lexically_normal();
        if (parent.filename().empty()) parent = parent.parent_path();
        const auto resolved = root.lexically_normal();
        if (resolved.parent_path().lexically_normal() == parent && resolved.filename().wstring().starts_with(L"pc-native-contract-")) {
            std::error_code error;
            std::filesystem::remove_all(resolved, error);
        }
    }
};
void Put(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << text;
}
std::vector<Object> ReadLines(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::vector<Object> lines;
    std::string line;
    while (std::getline(file, line)) lines.push_back(json::Parse(line));
    return lines;
}

void RedactionContract() {
    Redactor redactor;
    const auto payload = Make({{L"token", Text(L"synthetic-token")}, {L"includeSecrets", Boolean(false)},
        {L"nested", Text(L"{\"password\":\"synthetic-password\",\"ordinary\":42}")},
        {L"script", Text(L"$password = 'synthetic-password'; Write-Output 'safe'\nBearer synthetic-token\nhttps://fixture.invalid/?token=synthetic-token")}});
    const auto captured = json::WideToUtf8(std::wstring(redactor.Redact(payload, false).Stringify()));
    Require(captured.find("synthetic-token") == std::string::npos && captured.find("synthetic-password") == std::string::npos, "Secrets leaked through default redaction");
    Require(captured.find("Write-Output 'safe'") != std::string::npos, "Ordinary script body lost");
    Require(json::GetBool(redactor.Redact(payload, false).GetObject(), L"includeSecrets", true) == false, "Policy flag was redacted");
    Require(std::wstring(redactor.Redact(payload, true).Stringify()).find(L"synthetic-token") != std::wstring::npos, "Explicit secret opt-in not honored");
    const auto secure = Make({{L"header", Make({{L"action", Text(L"secureStorage.set")}})}, {L"body", Make({{L"message", Make({{L"value", Text(L"synthetic-value")}})}})}});
    Require(std::wstring(redactor.Redact(secure, false).Stringify()).find(L"synthetic-value") == std::wstring::npos, "Secure storage value leaked");
    const auto whitespaceJson = Text(L" \n {\"password\":\"opaque-secret\",\"auth\":\"opaque-auth\"}");
    const auto whitespaceCapture = std::wstring(redactor.Redact(whitespaceJson, false).Stringify());
    Require(whitespaceCapture.find(L"opaque-secret") == std::wstring::npos && whitespaceCapture.find(L"opaque-auth") == std::wstring::npos, "Whitespace embedded JSON secret leaked");
    const std::wstring safeScript = L"Write-Output 'safe'\n# " + std::wstring(100000, L'x');
    Require(redactor.RedactText(safeScript) == safeScript, "Long safe PowerShell script was truncated");
    Require(redactor.RedactText(L"$password = @'\nopaque-here-string\n'@\nWrite-Output 'safe'").find(L"opaque-here-string") == std::wstring::npos, "Here-string secret leaked");
    const auto common = redactor.RedactText(L"Authorization: Bearer opaque-auth\nBearer opaque-bearer\nhttps://user:opaque-userinfo@fixture.invalid/path?token=opaque-query\n-token 'opaque-argument'\neyJaaaa.eyJbbbb.opaque-jwt\n-----BEGIN RSA PRIVATE KEY-----\nopaque-private-key\n-----END RSA PRIVATE KEY-----");
    for (const auto secret : {L"opaque-auth", L"opaque-bearer", L"opaque-userinfo", L"opaque-query", L"opaque-argument", L"opaque-jwt", L"opaque-private-key"})
        Require(common.find(secret) == std::wstring::npos, "Common text credential form leaked");
    const auto whitespaceSchemes = redactor.RedactText(L"Authorization: Bearer\topaque-header\nBearer\topaque-standalone\nBasic\r\nopaque-basic");
    Require(whitespaceSchemes.find(L"opaque-") == std::wstring::npos, "Whitespace authentication scheme leaked");
    Require(redactor.RedactText(L"https://fixture.invalid?email=a@b") == L"https://fixture.invalid?email=a@b", "Query email mistaken for URL userinfo");
    std::wstring urlList;
    for (int index = 0; index < 10000; ++index) urlList += L"https://fixture.invalid/path\n";
    Require(redactor.RedactText(urlList) == urlList, "Large safe URL list changed");
}

void VdfContract() {
    const auto document = VdfNode::Parse(L"// fixture\n\"AppState\" { \"appid\" \"1\" \"name\" \"A \\\"quoted\\\" game\" }");
    Require(document.Child(L"AppState").Child(L"name").value == L"A \"quoted\" game", "VDF escaped name changed");
    bool rejected = false;
    try { (void)VdfNode::Parse(L"\"AppState\" { \"appid\" \"1\""); } catch (...) { rejected = true; }
    Require(rejected, "Malformed VDF accepted");
}

void InventoryContract() {
    Fixture fixture;
    const auto steam = fixture.root / L"Steam";
    const auto apps = steam / L"steamapps";
    std::filesystem::create_directories(apps / L"common" / L"Installed");
    std::filesystem::create_directories(apps / L"common" / L"Downloading");
    Put(apps / L"appmanifest_1.acf", "\"AppState\" { \"appid\" \"1\" \"name\" \"Installed fixture\" \"installdir\" \"Installed\" \"StateFlags\" \"4\" }");
    Put(apps / L"appmanifest_2.acf", "\"AppState\" { \"appid\" \"2\" \"name\" \"Downloading fixture\" \"installdir\" \"Downloading\" \"StateFlags\" \"2\" }");
    Put(apps / L"appmanifest_3.acf", "\"AppState\" { \"appid\" \"3\" \"name\" \"Escaped fixture\" \"installdir\" \"../../escape\" \"StateFlags\" \"4\" }");
    const auto epic = fixture.root / L"EpicManifests";
    const auto epicInstall = fixture.root / L"EpicGame";
    std::filesystem::create_directories(epicInstall);
    Put(epic / L"fixture.item", json::Stringify(Make({{L"AppName", Text(L"epic-fixture")}, {L"DisplayName", Text(L"Epic fixture")}, {L"InstallLocation", Text(epicInstall.wstring())}, {L"bIsIncompleteInstall", Boolean(false)}})));
    const auto inventory = GameInventory({steam}, epic).Capture();
    Require(inventory.games.size() == 2, "Installed manifest filtering incorrect");
    Require(GameInventory::ContainsExecutable(inventory.games[0].installRoot, inventory.games[0].installRoot / L"game.exe"), "Installed executable did not match");
    Require(!GameInventory::ContainsExecutable(inventory.games[0].installRoot, inventory.games[0].installRoot.wstring() + L"-other\\game.exe"), "Sibling installation incorrectly matched");
}

void WriterContract() {
    Fixture fixture;
    SessionLoggingConfig config;
    config.MaxFileBytes = 1000;
    config.MaxSessionBytes = 6000;
    Writer writer(config, fixture.root);
    const auto payload = Make({{L"event", Text(L"fixture")}, {L"password", Text(L"synthetic-secret")}, {L"body", Text(std::wstring(600, L'x'))}});
    Require(writer.Write(L"realtime", payload, false), "Initial log write failed");
    Require(writer.Write(L"realtime", payload, true), "Secret opt-in log write failed");
    Require(std::filesystem::exists(writer.Directory() / L"realtime.001.jsonl"), "Category rotation missing");
    const auto first = ReadLines(writer.Directory() / L"realtime.jsonl");
    const auto second = ReadLines(writer.Directory() / L"realtime.001.jsonl");
    Require(first.size() == 1 && !json::GetBool(first[0], L"includeSecrets", true) && json::GetBool(second[0], L"includeSecrets", false), "Log policy metadata incorrect");
    Require(json::GetString(json::GetObject(first[0], L"payload"), L"password") == L"[REDACTED]", "Disk boundary secret redaction missing");
    for (int index = 0; index < 20; ++index) writer.Write(L"realtime", payload, false);
    Require(writer.BytesWritten() <= static_cast<uint64_t>(config.MaxSessionBytes), "Session cap exceeded");
    const auto diagnostics = ReadLines(writer.Directory() / L"diagnostics.jsonl");
    Require(!diagnostics.empty() && json::GetString(json::GetObject(diagnostics.back(), L"payload"), L"event") == L"sessionStorageLimitReached", "Storage limit marker absent");
    Require(!writer.Write(L"realtime", payload, false), "Capture continued after session limit");
}

void ConfigContract() {
    AppConfig config;
    config.SessionLogging.SnapshotIntervalSeconds = -1;
    config.SessionLogging.MaxFileBytes = 1;
    config.SessionLogging.MaxSessionBytes = 1;
    config.SessionLogging.RetentionDays = 0;
    config.Normalize();
    Require(config.SessionLogging.SnapshotIntervalSeconds == 5 && config.SessionLogging.RetentionDays == 1, "Session configuration bounds absent");
    const auto output = json::GetObject(json::Parse(config.ToJson()), L"SessionLogging");
    Require(!json::GetBool(output, L"IncludeSecrets", true), "Process-lifetime opt-in persisted");
    Require(config.SessionLogging.MaxSessionBytes >= config.SessionLogging.MaxFileBytes + 4096, "Session cap below rotation cap");
}

void ProtocolContract() {
    const auto envelope = Make({{L"version", Number(1)}, {L"category", Text(L"realtime")}, {L"direction", Text(L"received")}, {L"timestamp", Number(1)}, {L"payload", Make({{L"type", Text(L"PROVISIONING_SCRIPT_DEPLOY")}})}});
    Require(DiagnosticPipe::Validate(envelope), "Valid Playcast frame rejected");
    envelope.Insert(L"version", Number(2));
    Require(!DiagnosticPipe::Validate(envelope), "Unknown protocol accepted");
    envelope.Insert(L"version", Number(1));
    envelope.Insert(L"category", Text(L"unknown"));
    Require(!DiagnosticPipe::Validate(envelope), "Unknown category accepted");
}

void ClassifierContract() {
    const auto wire = Make({{L"type", Text(L"PROVISIONING_SCRIPT_DEPLOY")}, {L"executionId", Text(L"fixture-execution")}, {L"completeScript", Text(L"Write-Output 'fixture'")}});
    const auto envelope = Make({{L"version", Number(1)}, {L"category", Text(L"realtime")}, {L"direction", Text(L"received")}, {L"timestamp", Number(1)}, {L"payload", wire}});
    const auto related = EventClassifier::RelatedRecords(envelope);
    Require(related.size() == 1 && related[0].first == L"scripts", "Provisioning packet classified more than once");
    Require(json::GetString(json::GetObject(related[0].second, L"message"), L"executionId") == L"fixture-execution", "Script correlation ID lost");
    const auto native = Make({{L"header", Make({{L"action", Text(L"enumRunningApplications")}})}, {L"body", Make({{L"applications", Array()}})}});
    envelope.Insert(L"payload", Text(json::Utf8ToWide(json::Stringify(native))));
    const auto games = EventClassifier::RelatedRecords(envelope);
    Require(games.size() == 1 && games[0].first == L"games" && json::Has(games[0].second.GetNamedObject(L"message"), L"body"), "Embedded native inventory lost reply body");
    envelope.Insert(L"payload", Make({{L"kind", Text(L"http_request")}, {L"method", Text(L"POST")}, {L"body", wire}}));
    const auto http = EventClassifier::RelatedRecords(envelope);
    Require(http.size() == 1 && http[0].first == L"scripts", "HTTP method prevented nested provisioning extraction");
}

void HeadlessLifecycleContract() {
    Fixture fixture;
    AppConfig config;
    std::filesystem::path directory;
    {
        SessionLogger logger(config, true, false, fixture.root);
        Require(logger.Status().find(L"Passive guest capture") != std::wstring::npos, "Headless capture coverage missing");
        logger.UpdateSession(true, L"fixture active", L"fixture-guest-session");
        directory = logger.Directory();
        Require(directory.parent_path() == fixture.root && std::filesystem::exists(directory / L"session.jsonl"), "Headless guest did not open session log");
        Require(logger.Status().find(L"Session logging active") != std::wstring::npos, "Headless guest active status missing");
        logger.UpdateSession(false, L"fixture ended", L"fixture-guest-session");
        logger.Stop();
    }
    const auto session = ReadLines(directory / L"session.jsonl");
    Require(session.size() == 2, "Headless guest lifecycle records missing");
    Require(json::GetString(json::GetObject(session.front(), L"payload"), L"event") == L"sessionStarted", "Headless guest start missing");
    Require(json::GetString(json::GetObject(session.back(), L"payload"), L"event") == L"sessionEnded", "Headless guest end missing");
    Require(json::GetString(json::GetObject(session.front(), L"payload"), L"coverage").find(L"owner Companion is required") != std::wstring::npos, "Guest feed owner requirement absent");
    Require(!std::filesystem::exists(directory / L"realtime.jsonl"), "Guest passive capture claimed realtime messages");

    const auto harnessRoot = fixture.root / L"harness";
    SessionLogger harness(config, true, true, harnessRoot);
    harness.UpdateSession(true, L"fixture preview", L"fixture-guest-session");
    harness.Stop();
    Require(!std::filesystem::exists(harnessRoot), "Harness preview wrote session files");
}
void PipeContract() {
    std::mutex mutex;
    std::condition_variable changed;
    int received = 0;
    std::wstring lastStatus;
    const auto name = std::format(L"PlaycastCompanion.Contracts.{}", GetCurrentProcessId());
    DiagnosticPipe server(L"NonsoleMode", [&](Object packet) {
        std::lock_guard lock(mutex);
        if (json::GetString(packet, L"category") == L"realtime") ++received;
        changed.notify_all();
    }, [&](std::wstring text) { std::lock_guard lock(mutex); lastStatus = std::move(text); changed.notify_all(); }, name);
    const auto connect = [&]() {
        const auto path = L"\\\\.\\pipe\\" + name;
        for (int attempt = 0; attempt < 80; ++attempt) {
            UniqueHandle client(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (client.valid()) return client;
            WaitNamedPipeW(path.c_str(), 100);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        throw std::runtime_error("Isolated test pipe unavailable");
    };
    const auto frame = json::Stringify(Make({{L"version", Number(1)}, {L"category", Text(L"realtime")}, {L"direction", Text(L"received")}, {L"timestamp", Number(1)}, {L"payload", Make({{L"type", Text(L"fixture")}})}})) + "\n";
    const auto send = [&](HANDLE client, std::string_view bytes) { DWORD written = 0; Require(WriteFile(client, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size(), "Test frame write failed"); };
    {
        auto client = connect();
        send(client.get(), "{malformed}\n");
        send(client.get(), frame.substr(0, 10));
        send(client.get(), frame.substr(10));
        std::unique_lock lock(mutex);
        Require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return received == 1; }), "Fragmented frame was not delivered");
    }
    {
        auto client = connect();
        send(client.get(), frame);
        std::unique_lock lock(mutex);
        Require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return received == 2; }), "Reconnect did not deliver frame");
    }
    const auto before = std::chrono::steady_clock::now();
    server.Stop();
    Require(std::chrono::steady_clock::now() - before < std::chrono::seconds(3), "Idle pipe shutdown did not cancel promptly");
}
}

int wmain() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
        int count = 0;
        for (const auto& [name, test] : std::initializer_list<std::pair<const char*, void(*)()>>{
            {"redaction", RedactionContract}, {"VDF", VdfContract}, {"inventory", InventoryContract}, {"writer", WriterContract},
            {"config", ConfigContract}, {"protocol", ProtocolContract}, {"classifier", ClassifierContract}, {"pipe", PipeContract},
            {"headless lifecycle", HeadlessLifecycleContract}}) {
            test(); ++count; std::cout << "PASS " << name << '\n';
        }
        std::cout << count << " native logger contracts passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    catch (const winrt::hresult_error& error) { std::cerr << "FAIL WinRT: " << json::WideToUtf8(error.message().c_str()) << '\n'; return 1; }
}
