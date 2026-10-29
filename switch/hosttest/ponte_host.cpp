// O cliente do Switch, rodando no PC: o mesmo núcleo do protocolo que vai no
// fork do Sphaira (utils/ponte_proto.cpp), falando HTTP do jeito que o console
// fala (mesmos cabeçalhos, mesmo Range, mesma ordem de chamadas).
//
//   ponte_host unit
//   ponte_host discover <ip> <porta_udp>
//   ponte_host flow <ip> <porta> <codigo> <pasta_destino> [instalado ...]
//
// "instalado" é TITLEID:versão:conteúdo, ex.: 0100ABCD12340000:0:base.
// A saída é uma linha por fato, para o pytest conferir (test_switch_host.py).

#include "utils/ponte_proto.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace sphaira::ponte;

namespace {

int g_failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FALHOU %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

struct Response {
    int status{};
    std::map<std::string, std::string> headers{};
    std::string body{};
};

// HTTP/1.1 mínimo: a SPEC garante Content-Length em toda resposta (§2).
// `abort_after` >= 0 fecha a conexão depois de ler esse tanto do corpo,
// imitando o console caindo da rede no meio de um download.
auto Request(const std::string& ip, int port, const std::string& method, const std::string& path,
             const std::vector<std::string>& headers, const std::string& body = {}, long long abort_after = -1) -> Response {
    Response out{};
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return out;
    }

    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + ip + "\r\nConnection: close\r\n";
    for (const auto& h : headers) {
        req += h + "\r\n";
    }
    if (method == "POST") {
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n" + body;
    send(fd, req.data(), req.size(), 0);

    std::string raw;
    char buf[65536];
    size_t header_end = std::string::npos;
    long long content_length = -1;
    for (;;) {
        const auto n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        raw.append(buf, n);
        if (header_end == std::string::npos) {
            header_end = raw.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::sscanf(raw.c_str(), "HTTP/1.1 %d", &out.status);
                size_t pos = raw.find("\r\n") + 2;
                while (pos < header_end) {
                    const auto eol = raw.find("\r\n", pos);
                    const auto line = raw.substr(pos, eol - pos);
                    const auto colon = line.find(':');
                    if (colon != std::string::npos) {
                        auto key = line.substr(0, colon);
                        for (auto& c : key) c = static_cast<char>(std::tolower(c));
                        auto value = line.substr(colon + 1);
                        while (!value.empty() && value.front() == ' ') value.erase(0, 1);
                        out.headers[key] = value;
                    }
                    pos = eol + 2;
                }
                if (out.headers.contains("content-length")) {
                    content_length = std::stoll(out.headers["content-length"]);
                }
            }
        }
        if (header_end != std::string::npos) {
            const auto got = static_cast<long long>(raw.size() - header_end - 4);
            if (abort_after >= 0 && got >= abort_after) {
                raw.resize(header_end + 4 + abort_after);
                break;
            }
            if (content_length >= 0 && got >= content_length) {
                break;
            }
        }
    }
    close(fd);

    if (header_end != std::string::npos) {
        out.body = raw.substr(header_end + 4);
    }
    return out;
}

void Unit() {
    // conteúdo pelo Title ID (SPEC §1.2)
    CHECK(ContentOfTitleId(0x0100ABCD12340000ULL) == Content::Base);
    CHECK(ContentOfTitleId(0x0100ABCD12340800ULL) == Content::Update);
    CHECK(ContentOfTitleId(0x0100ABCD12341001ULL) == Content::Dlc);
    CHECK(ContentOfTitleId(0x0100ABCD12340001ULL) == Content::Unknown);
    CHECK(FormatTitleId(0x0100abcd12340800ULL) == "0100ABCD12340800");
    std::uint64_t tid{};
    CHECK(ParseTitleId("0100abcd12340000", tid) && tid == 0x0100ABCD12340000ULL);
    CHECK(!ParseTitleId("0100abcd1234000", tid));
    CHECK(!ParseTitleId("0100abcd1234000g", tid));

    // caminhos (SPEC §5.4)
    CHECK(IsSafePath("jogo.nsp"));
    CHECK(IsSafePath("sce_sys/param.json"));
    CHECK(!IsSafePath(""));
    CHECK(!IsSafePath("/etc/passwd"));
    CHECK(!IsSafePath("../x"));
    CHECK(!IsSafePath("a/../b"));
    CHECK(!IsSafePath("a//b"));
    CHECK(!IsSafePath("a/"));
    CHECK(!IsSafePath("a\\b"));
    CHECK(!IsSafePath("./a"));

    // firmware e espaço
    CHECK(FormatFirmware(19, 0) == "19.00");
    CHECK(FormatFirmware(9, 2) == "9.02");
    CHECK(Fits(100, 105));
    CHECK(!Fits(100, 104));
    CHECK(Fits(0, 0));
    CHECK(Fits(48318382080LL, 50734301184LL));
    CHECK(!Fits(48318382080LL, 50734301183LL));

    CHECK(BaseUrl("192.168.0.10") == "http://192.168.0.10:47101");
    CHECK(BaseUrl(" 192.168.0.10:8080/ ") == "http://192.168.0.10:8080");
    CHECK(BaseUrl("http://mac.local") == "http://mac.local:47101");
    CHECK(BaseUrl("https://mac.local").empty());
    CHECK(BaseUrl("mac.local/x").empty());
    CHECK(BaseUrl("").empty());

    // erros e respostas
    ApiError err{};
    CHECK(ParseError(R"({"error":{"code":"item_not_ready","message":"Ainda extraindo"}})", err));
    CHECK(err.code == "item_not_ready" && err.message == "Ainda extraindo");
    CHECK(!ParseError(R"({"items":[]})", err));
    CHECK(!ParseError("não é json", err));

    DiscoveryReply d{};
    CHECK(ParseDiscoveryReply(R"({"ponte":1,"name":"MacBook do João","port":47101,"id":"b3f1c2d4e5f60718"})", d));
    CHECK(d.port == 47101 && d.name == "MacBook do João" && d.id == "b3f1c2d4e5f60718");
    CHECK(!ParseDiscoveryReply(R"({"ponte":1,"port":70000,"id":"x"})", d));
    CHECK(!ParseDiscoveryReply(R"({"magic":"OWNFOIL"})", d));

    std::string token;
    CHECK(ParsePairReply(R"({"client_token":"c9a1"})", token) && token == "c9a1");
    CHECK(ParsePairReply(R"({"client_token":""})", token) && token.empty());

    Changes ch{};
    CHECK(ParseChanges(R"({"changed":true,"new_items":2,"generated_at":"2026-10-28T18:20:00Z"})", ch));
    CHECK(ch.changed && ch.new_items == 2 && ch.generated_at == "2026-10-28T18:20:00Z");
    CHECK(!ParseChanges(R"({"new_items":2})", ch));

    // detalhe com caminho malicioso é recusado inteiro
    ItemDetail det{};
    CHECK(!ParseItemDetail(R"({"id":"8f3e0a1c9b2d4e57","title":"x","files":[{"index":0,"path":"../../x","size":1}]})", det));
    CHECK(ParseItemDetail(R"({"id":"8f3e0a1c9b2d4e57","title":"x","kind":"nsp","files":[{"index":0,"path":"x.nsp","size":10,"sha256":null}]})", det));
    CHECK(det.files.size() == 1 && det.files[0].sha256.empty() && det.item.kind == "nsp");

    // catálogo nulo onde a SPEC permite; item quebrado não derruba o resto
    Catalog cat{};
    CHECK(ParseCatalog(R"({"generated_at":"2026-10-28T18:10:00Z","items":[
        {"id":"0000000000000001","title":"Jogo B","title_id":"0100abcd12340000","content":"base","kind":"nsp","version":"0","size":10,"state":"installed"},
        {"id":"0000000000000002","title":"Jogo B","title_id":"0100ABCD12340800","parent_title_id":"0100abcd12340000","content":"update","kind":"nsp","version":"65536","size":10,"state":"update_available"},
        {"id":"0000000000000003","title":"Jogo B","title_id":"0100ABCD12341001","parent_title_id":"0100ABCD12340000","content":"dlc","kind":"nsz","version":"0","size":10,"state":"new"},
        {"id":"0000000000000004","title":"Jogo A","title_id":null,"content":"unknown","kind":"xci","version":null,"size":10,"icon":null,"state":"new"},
        {"id":"0000000000000005","title":"Jogo B","title_id":"0100ABCD12340800","parent_title_id":"0100ABCD12340000","content":"update","kind":"nsp","version":"131072","size":10,"state":"update_available"},
        {"id":"0000000000000006","title":"Outro","title_id":"0100FFFF00001001","parent_title_id":"0100FFFF00000000","content":"dlc","kind":"nsp","version":"0","size":10,"state":"new"},
        {"id":"curto","title":"quebrado"}
    ]})", cat));
    CHECK(cat.items.size() == 6);
    CHECK(cat.items[0].title_id == "0100ABCD12340000"); // normalizado em maiúsculas
    CHECK(cat.items[3].title_id.empty() && cat.items[3].version.empty() && cat.items[3].icon.empty());
    CHECK(cat.items[1].state == State::UpdateAvailable);

    // agrupado: Jogo A, Jogo B (base, update mais novo, update velho, dlc), Outro (dlc órfão)
    const auto rows = GroupRows(cat.items);
    CHECK(rows.size() == 6);
    if (rows.size() == 6) {
        CHECK(rows[0].item == 3 && rows[0].depth == 0);
        CHECK(rows[1].item == 0 && rows[1].depth == 0);
        CHECK(rows[2].item == 4 && rows[2].depth == 1);
        CHECK(rows[3].item == 1 && rows[3].depth == 1);
        CHECK(rows[4].item == 2 && rows[4].depth == 1);
        CHECK(rows[5].item == 5 && rows[5].depth == 0);
    }

    // ordem de instalação: base → updates do mais velho ao mais novo → dlc
    const auto order = InstallOrder(cat.items, {2, 4, 0, 1, 3});
    CHECK((order == std::vector<std::size_t>{3, 0, 1, 4, 2}));

    // companheiros de um jogo: o update mais novo e o dlc que ainda não estão
    const auto comp = Companions(cat.items, 0);
    CHECK((comp == std::vector<std::size_t>{0, 2, 4}));

    // corpos enviados
    ConsoleState st{};
    st.firmware = "19.00";
    st.storages.push_back({"sd", "microSD", 1000, 2000});
    st.installed.push_back({"0100ABCD12340000", "0", Content::Base, "sd"});
    const auto body = BuildConsoleState(st);
    CHECK(body.find(R"("platform":"switch")") != std::string::npos);
    CHECK(body.find(R"("content":"base")") != std::string::npos);
    CHECK(body.find(R"("free":1000)") != std::string::npos);
    const auto report = BuildReport({ReportResult::Cancelled, "sd", {}, 3});
    CHECK(report == R"({"result":"cancelled","storage":"sd","detail":null,"duration_s":3})");
    const auto pair = BuildPairBody("123456", "Switch \"sala\"", "0.1.0", "19.00");
    CHECK(pair == R"({"code":"123456","client":{"platform":"switch","name":"Switch \"sala\"","version":"0.1.0","firmware":"19.00"}})");

    CHECK(InstallPathFor(cat.items[2]) == "ponte.nsz");
    CHECK(RouteFile("8f3e0a1c9b2d4e57", 0) == "/api/v1/items/8f3e0a1c9b2d4e57/files/0");

    std::printf(g_failures ? "UNIT FALHOU %d\n" : "UNIT OK\n", g_failures);
}

int Discover(const std::string& ip, int port) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    sendto(fd, DISCOVERY_QUERY, std::strlen(DISCOVERY_QUERY), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, 1000) <= 0) {
        std::printf("DISCOVER nenhum\n");
        close(fd);
        return 1;
    }
    char buf[1024];
    sockaddr_in from{};
    socklen_t len = sizeof(from);
    const auto n = recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &len);
    close(fd);
    DiscoveryReply reply{};
    if (n <= 0 || !ParseDiscoveryReply(std::string_view{buf, static_cast<size_t>(n)}, reply)) {
        std::printf("DISCOVER invalido\n");
        return 1;
    }
    char host[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host));
    std::printf("DISCOVER %s %d %s %s\n", host, reply.port, reply.id.c_str(), reply.name.c_str());
    return 0;
}

int Flow(const std::string& ip, int port, const std::string& code, const std::string& dest, const std::vector<std::string>& installed_args) {
    const std::string proto = "X-Ponte-Protocol: " + std::to_string(PROTOCOL_VERSION);

    ServerInfo info{};
    auto r = Request(ip, port, "GET", RouteInfo(), {proto});
    if (r.status != 200 || !ParseInfo(r.body, info)) {
        std::printf("INFO falhou %d\n", r.status);
        return 1;
    }
    std::printf("INFO %s pairing=%d\n", info.id.c_str(), info.pairing_required);

    // código errado primeiro: 401 com o corpo de erro da SPEC
    r = Request(ip, port, "POST", RoutePair(), {proto}, BuildPairBody("000000" == code ? "111111" : "000000", "Switch de teste", "0.1.0", "19.00"));
    ApiError err{};
    std::printf("PAIR_ERRADO %d %s\n", r.status, ParseError(r.body, err) ? err.code.c_str() : "-");

    r = Request(ip, port, "POST", RoutePair(), {proto}, BuildPairBody(code, "Switch de teste", "0.1.0", "19.00"));
    std::string token;
    if (r.status != 200 || !ParsePairReply(r.body, token)) {
        std::printf("PAIR falhou %d\n", r.status);
        return 1;
    }
    std::printf("PAIR ok\n");
    const std::string auth = "Authorization: Bearer " + token;

    ConsoleState state{};
    state.firmware = "19.00";
    state.storages.push_back({"nand", "Console", 20LL << 30, 26LL << 30});
    state.storages.push_back({"sd", "microSD", 400LL << 30, 512LL << 30});
    for (const auto& arg : installed_args) {
        const auto a = arg.find(':');
        const auto b = arg.find(':', a + 1);
        state.installed.push_back({arg.substr(0, a), arg.substr(a + 1, b - a - 1), ParseContent(arg.substr(b + 1)), "sd"});
    }

    r = Request(ip, port, "POST", RouteConsoleState(), {proto, auth}, BuildConsoleState(state));
    Catalog catalog{};
    if (r.status != 200 || !ParseCatalog(r.body, catalog)) {
        std::printf("STATE falhou %d\n", r.status);
        return 1;
    }
    r = Request(ip, port, "GET", RouteChanges(catalog.generated_at), {proto, auth});
    Changes changes{};
    std::printf("CHANGES %d %d %lld\n", r.status, ParseChanges(r.body, changes) ? changes.changed : -1, static_cast<long long>(changes.new_items));

    for (const auto& row : GroupRows(catalog.items)) {
        const auto& it = catalog.items[row.item];
        std::printf("ROW %d %s %s %s %s %s\n", row.depth, it.title_id.empty() ? "-" : it.title_id.c_str(),
                    ContentName(it.content), it.version.empty() ? "-" : it.version.c_str(), StateName(it.state), it.kind.c_str());
    }

    // o que o console instalaria: tudo que não está instalado nem é incompatível
    std::vector<std::size_t> chosen;
    for (std::size_t i = 0; i < catalog.items.size(); i++) {
        if (catalog.items[i].state == State::New || catalog.items[i].state == State::UpdateAvailable) {
            chosen.push_back(i);
        }
    }

    for (const auto i : InstallOrder(catalog.items, chosen)) {
        const auto& item = catalog.items[i];
        r = Request(ip, port, "GET", RouteItem(item.id), {proto, auth});
        ItemDetail detail{};
        if (r.status != 200 || !ParseItemDetail(r.body, detail)) {
            std::printf("ITEM falhou %s %d\n", item.id.c_str(), r.status);
            return 1;
        }

        for (const auto& file : detail.files) {
            const auto path = RouteFile(item.id, file.index);
            const std::string rev = "X-Ponte-Revision: " + detail.item.revision;

            // revisão errada → 412 revision_changed
            r = Request(ip, port, "GET", path, {proto, auth, "Range: bytes=0-", "X-Ponte-Revision: 00000000"});
            std::printf("STALE %d %s\n", r.status, ParseError(r.body, err) ? err.code.c_str() : "-");

            // o jeito do yati::source::Http: Range aberto, cai no meio, retoma
            const auto half = file.size / 2;
            r = Request(ip, port, "GET", path, {proto, auth, rev, "Range: bytes=0-"}, {}, half);
            std::string data = r.body;
            const auto first_status = r.status;
            r = Request(ip, port, "GET", path, {proto, auth, rev, "Range: bytes=" + std::to_string(data.size()) + "-"});
            data += r.body;
            std::printf("FILE %s %d %d %zu/%lld %s\n", item.kind.c_str(), first_status, r.status, data.size(),
                        static_cast<long long>(file.size), r.headers["content-range"].c_str());

            std::ofstream{dest + "/" + item.id + "." + item.kind, std::ios::binary}.write(data.data(), data.size());
        }

        r = Request(ip, port, "POST", RouteReport(item.id), {proto, auth}, BuildReport({ReportResult::Installed, "sd", {}, 1}));
        std::printf("REPORT %s %d\n", item.id.c_str(), r.status);
    }

    // protocolo de outra versão major → 426
    r = Request(ip, port, "GET", RouteInfo(), {"X-Ponte-Protocol: 2"});
    std::printf("MISMATCH %d %s\n", r.status, ParseError(r.body, err) ? err.code.c_str() : "-");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "unit") {
        Unit();
        return g_failures ? 1 : 0;
    }
    if (cmd == "discover" && argc == 4) {
        return Discover(argv[2], std::atoi(argv[3]));
    }
    if (cmd == "flow" && argc >= 6) {
        return Flow(argv[2], std::atoi(argv[3]), argv[4], argv[5], std::vector<std::string>(argv + 6, argv + argc));
    }
    std::fprintf(stderr, "uso: ponte_host unit | discover <ip> <porta> | flow <ip> <porta> <codigo> <dest> [TID:ver:conteudo ...]\n");
    return 2;
}
