// UPnP on the router (upnp.h): SSDP finds the Internet Gateway Device, its description names the WANIPConnection or
// WANPPPConnection control URL, and SOAP calls there read the public address and add the port mapping.
#include "upnp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "link.h"

#define SSDP_MS 2000
#define HTTP_MS 3000
#define ERR_CONFLICT 718
#define ERR_PERMANENT_ONLY 725

static Ps3NetAddr control_at;
static char control_path[160], service[96];
static bool found, added;
static char buf[16384];

// "http://a.b.c.d[:port]/path": the address and the path. Routers name themselves by IP.
static bool parse_url(const char* url, Ps3NetAddr* at, char* path, size_t cap)
{
    unsigned a, b, c, d, port = 80;
    const char* slash;

    if (strncasecmp(url, "http://", 7) != 0 || sscanf(url + 7, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false;
    }
    slash = strchr(url + 7, '/');
    sscanf(url + 7, "%*[0-9.]:%u", &port);
    at->ip = a << 24 | b << 16 | c << 8 | d;
    at->port = (uint16_t) port;
    snprintf(path, cap, "%s", slash != NULL ? slash : "/");
    return true;
}

// The text between <name> and </name> from p on; NULL when absent.
static const char* tag(const char* p, const char* name, char* out, size_t cap)
{
    char open[64], close[64];
    const char* s;
    const char* e;
    size_t n;

    snprintf(open, sizeof(open), "<%s>", name);
    snprintf(close, sizeof(close), "</%s>", name);
    if ((s = strstr(p, open)) == NULL || (e = strstr(s, close)) == NULL) {
        return NULL;
    }
    s += strlen(open);
    n = (size_t) (e - s) < cap - 1 ? (size_t) (e - s) : cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
    return e;
}

static int http(const Ps3NetAddr* at, const char* req)
{
    int n = ps3_link_tcp_ask(at, req, (int) strlen(req), buf, (int) sizeof(buf) - 1, HTTP_MS);

    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    return n;
}

static bool discover(void)
{
    static const char search[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                                 "MX: 2\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
    static const Ps3NetAddr ssdp = { 0xEFFFFFFAu, 1900 };
    Ps3NetAddr from, desc_at;
    char url[192], path[160], req[320], type[96], ctl[160];
    const char* p;
    int n;

    n = ps3_link_udp_ask(&ssdp, search, (int) sizeof(search) - 1, buf, (int) sizeof(buf) - 1, SSDP_MS, &from);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';
    p = buf;
    while (p != NULL && strncasecmp(p, "LOCATION:", 9) != 0) {
        p = strchr(p, '\n');
        p = p != NULL ? p + 1 : NULL;
    }
    if (p == NULL || sscanf(p + 9, " %191s", url) != 1 || !parse_url(url, &desc_at, path, sizeof(path))) {
        return false;
    }
    snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %u.%u.%u.%u:%u\r\nConnection: close\r\n\r\n", path,
             desc_at.ip >> 24, (desc_at.ip >> 16) & 255, (desc_at.ip >> 8) & 255, desc_at.ip & 255, desc_at.port);
    if (http(&desc_at, req) <= 0) {
        return false;
    }
    for (p = buf; (p = tag(p, "serviceType", type, sizeof(type))) != NULL;) {
        if ((strstr(type, "WANIPConnection") != NULL || strstr(type, "WANPPPConnection") != NULL) &&
            tag(p, "controlURL", ctl, sizeof(ctl)) != NULL)
        {
            snprintf(service, sizeof(service), "%s", type);
            control_at = desc_at;
            if (ctl[0] == '/') {
                snprintf(control_path, sizeof(control_path), "%s", ctl);
                return true;
            }
            return parse_url(ctl, &control_at, control_path, sizeof(control_path));
        }
    }
    return false;
}

// A SOAP call on the control URL: the HTTP status, the answer in buf, and a fault's UPnP error in *error.
static int soap(const char* action, const char* args, int* error)
{
    static char body[1024], req[1792];
    const Ps3NetAddr* at = &control_at;
    char code[16], status[8];

    snprintf(body, sizeof(body),
             "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s>"
             "</s:Body></s:Envelope>\r\n",
             action, service, args, action);
    snprintf(req, sizeof(req),
             "POST %s HTTP/1.0\r\nHost: %u.%u.%u.%u:%u\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
             "Content-Length: %d\r\nSOAPAction: \"%s#%s\"\r\nConnection: close\r\n\r\n%s",
             control_path, at->ip >> 24, (at->ip >> 16) & 255, (at->ip >> 8) & 255, at->ip & 255, at->port,
             (int) strlen(body), service, action, body);
    *error = 0;
    if (http(at, req) <= 0 || sscanf(buf, "HTTP/%*s %7s", status) != 1) {
        return -1;
    }
    if (tag(buf, "errorCode", code, sizeof(code)) != NULL) {
        *error = atoi(code);
    }
    return atoi(status);
}

static void dotted(uint32_t ip, char* out, size_t cap)
{
    snprintf(out, cap, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

void ps3_upnp_open(uint16_t port, uint32_t local_ip, uint32_t lease_s, Ps3Upnp* out)
{
    char args[640], ip[20], value[32];
    unsigned a, b, c, d;
    int status, error;

    memset(out, 0, sizeof(*out));
    if (!found && !(found = discover())) {
        return;
    }
    out->router = true;
    if (soap("GetExternalIPAddress", "", &error) == 200 && tag(buf, "NewExternalIPAddress", value, sizeof(value)) &&
        sscanf(value, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
    {
        out->public_ip = a << 24 | b << 16 | c << 8 | d;
    }
    dotted(local_ip, ip, sizeof(ip));
    for (;;) {
        snprintf(args, sizeof(args),
                 "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>"
                 "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient>"
                 "<NewEnabled>1</NewEnabled><NewPortMappingDescription>Melee netplay</NewPortMappingDescription>"
                 "<NewLeaseDuration>%u</NewLeaseDuration>",
                 port, port, ip, lease_s);
        status = soap("AddPortMapping", args, &error);
        if (error == ERR_PERMANENT_ONLY && lease_s != 0) {
            lease_s = 0;
            continue;
        }
        break;
    }
    if (status == 200) {
        out->opened = added = true;
    } else if (error == ERR_CONFLICT) {
        // Taken: by a forward to this console it is open all the same.
        snprintf(args, sizeof(args),
                 "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>",
                 port);
        out->opened = soap("GetSpecificPortMappingEntry", args, &error) == 200 &&
                      tag(buf, "NewInternalClient", value, sizeof(value)) && strcmp(value, ip) == 0;
    }
}

void ps3_upnp_close(uint16_t port)
{
    char args[256];
    int error;

    if (!added) {
        return;
    }
    added = false;
    snprintf(args, sizeof(args),
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>",
             port);
    soap("DeletePortMapping", args, &error);
}
