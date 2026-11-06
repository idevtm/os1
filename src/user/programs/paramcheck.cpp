#include <os1/parameter_client.hpp>

#include "interface/parameter_demo.hpp"
using namespace os1::user;
namespace p = os1::parameters;
namespace demo = os1::parameter_demo;
static p::Metadata schema, scratch;
static p::Diagnostic diagnostic;
static ParameterClient client;
#define CHECK(c, text)                                                      \
    do                                                                      \
    {                                                                       \
        if(!(c))                                                            \
        {                                                                   \
            constexpr char message[] = "[user/paramcheck] fail " text "\n"; \
            write(2, message, sizeof(message) - 1);                         \
            client.stop();                                                  \
            return 1;                                                       \
        }                                                                   \
    } while(0)
void say(const char* s)
{
    unsigned n = 0;
    while(s[n])
        ++n;
    write(1, s, n);
}
unsigned field(const os1_constructor_args& a, unsigned wanted)
{
    unsigned offset = 2;
    while(offset + 6 <= a.size)
    {
        unsigned id = os1::constructor::decode(a.payload + offset, 4);
        unsigned size = os1::constructor::decode(a.payload + offset + 4, 2);
        if(id == wanted)
            return offset + 6;
        offset += 6 + size;
    }
    return 256;
}
bool call_ok(const os1_constructor_args& args, uint64_t count, const char* text)
{
    os1_ipc_result result{};
    unsigned size = 0;
    while(text[size])
        ++size;
    if(client.call(demo::Render, args.payload, args.size, result) < 0 || result.transport_status ||
       result.application_status || result.payload_bytes != size + 8 ||
       os1::constructor::decode(result.payload, 8) != count)
        return false;
    for(unsigned i = 0; i < size; ++i)
        if(result.payload[8 + i] != static_cast<uint8_t>(text[i]))
            return false;
    return true;
}
bool rejected(const os1_constructor_args& args)
{
    os1_ipc_result result{};
    return client.call(demo::Render, args.payload, args.size, result) == 0 &&
           !result.transport_status && result.application_status == OS1_ERR_INVALID &&
           result.payload_bytes == 0;
}
int main()
{
    long image = open("/disk/bin/paramdemo",
                      OS1_RIGHT_EXECUTE | OS1_RIGHT_READ | OS1_RIGHT_SEEK | OS1_RIGHT_INSPECT);
    CHECK(image >= 0, "image");
    CHECK(parameter_metadata(image, schema, scratch) == 1, "retained metadata");
    CHECK(client.start(image, schema, diagnostic) == 0, "start");
    os1_constructor_args args{}, changed{};
    CHECK(parameter_arguments(schema, "", args, diagnostic) && call_ok(args, 1, "!"), "defaults");
    CHECK(parameter_arguments(schema,
                              "initial:4 ceiling:8 mode.thorough source.text:'ab c' "
                              "options:{prefix:'>', suffix:null} label:\"L \"",
                              args,
                              diagnostic) &&
              call_ok(args, 8, "L >AB C"),
          "quoted struct optional enum");
    CHECK(parameter_arguments(schema,
                              "mode:none source.file:{path:'ABC', offset:1} options.fallback.fast "
                              "options.suffix:null",
                              args,
                              diagnostic) &&
              call_ok(args, 2, "bc"),
          "file text fallback");
    CHECK(parameter_arguments(
              schema, "label:\"null\" source.none options.suffix:null", args, diagnostic) &&
              call_ok(args, 4, "null"),
          "literal null");
    CHECK(parameter_arguments(
              schema, "label:null source.none options.suffix:null", args, diagnostic) &&
              call_ok(args, 0, ""),
          "optional null unit");
    CHECK(parameter_arguments(
              schema, "source.text:'AbC' options:{enabled:false,limit:2}", args, diagnostic) &&
              call_ok(args, 3, "Ab!"),
          "enabled limit");
    CHECK(parameter_arguments(schema, "", args, diagnostic), "wire baseline");
    changed = args;
    changed.payload[field(changed, 3)] = 99;
    CHECK(rejected(changed), "unknown wire enum");
    changed = args;
    changed.payload[field(changed, 4)] = 99;
    CHECK(rejected(changed), "unknown wire case");
    changed = args;
    changed.payload[field(changed, 6)] = 2;
    CHECK(rejected(changed), "optional tag");
    changed = args;
    --changed.size;
    CHECK(rejected(changed), "truncated wire");
    changed = args;
    changed.payload[changed.size++] = 0;
    CHECK(rejected(changed), "trailing wire");
    changed = args;
    changed.payload[2] = 2;
    CHECK(rejected(changed), "duplicate wire field");
    CHECK(parameter_arguments(schema, "initial:101", changed, diagnostic) && rejected(changed),
          "application invariant");
    CHECK(call_ok(args, 1, "!"), "no state damage after rejection");
    // Find an exactly full canonical transport using public normalization.
    char raw[400]{};
    unsigned n = 0;
    auto add = [&](const char* s) {
        while(*s)
            raw[n++] = *s++;
    };
    add("source.text:'");
    for(unsigned i = 0; i < 64; ++i)
        raw[n++] = 'A';
    add("' options.prefix:'");
    for(unsigned i = 0; i < 64; ++i)
        raw[n++] = 'p';
    add("' label:'");
    unsigned label = n;
    bool full = false;
    for(unsigned length = 0; length <= 64; ++length)
    {
        n = label;
        for(unsigned i = 0; i < length; ++i)
            raw[n++] = 'l';
        raw[n++] = '\'';
        raw[n] = 0;
        if(parameter_arguments(schema, raw, changed, diagnostic) && changed.size == 256)
        {
            full = true;
            break;
        }
    }
    CHECK(full, "full wire fixture");
    os1_ipc_result result{};
    CHECK(client.call(demo::Render, changed.payload, changed.size, result) == 0 &&
              !result.transport_status && !result.application_status,
          "256 byte provider decode");
    CHECK(client.call(demo::Render, changed.payload, 257, result) == OS1_ERR_INVALID,
          "oversize admission");
    client.stop();
    // The provider independently checks constructor identity and bytes too.
    changed = args;
    changed.payload[field(changed, 3)] = 99;
    CHECK(client.start(image, schema, diagnostic, &changed) < 0, "constructor wire rejection");
    changed = args;
    ++changed.schema_id;
    CHECK(client.start(image, schema, diagnostic, &changed) < 0, "constructor identity rejection");
    close(image);
    say("[user/paramcheck] malformed enum variant optional wire rejected\n[user/paramcheck] ok\n");
    return 0;
}
