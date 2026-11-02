#include "ChakraCore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

// b5-guid-rotation demo host (ChakraCore API of this tree):
//   serialize <file>  - JsSerialize the script, dump the cache buffer
//   run <file>        - JsRunSerialized the cache file
// A cache whose version stamps (GUID DWORDs under the release scheme)
// don't match the engine must fail with JsErrorBadSerializedScript
// (ByteCodeSerializer.cpp:3099-3171 -> Jsrt.cpp:4083-4086), never misread.
//
// Engine debug flags are not parsed from this process's argv by default;
// the shell (bin/ch/ChakraRtInterface.cpp:254) feeds argv through the
// test hook SetConfigFlags (bin/ChakraCore/TestHooks.cpp:32). This host
// calls the same exported hook with its own argv, so
//   ./demo <mode> <file> -ForceSerializedBytecodeVersionSchema:21
// selects the release (GUID) scheme (21 = non-NTBUILD ReleaseVersioningScheme,
// ByteCodeSerializer.cpp:113-114).

typedef int (*SetConfigFlagsFn)(int, uint16_t**, void*);

static void ForwardFlagsToEngine(int argc, char** argv)
{
    // Only flag tokens (the parser accepts at most one bare filename, and
    // this host's mode/file args would collide as duplicate entries).
    const char* flagArgs[16];
    int n = 0;
    flagArgs[n++] = argv[0];
    for (int i = 3; i < argc && n < 16; i++)
    {
        if (argv[i][0] == '-' || argv[i][0] == '/')
        {
            flagArgs[n++] = argv[i];
        }
    }

    SetConfigFlagsFn setFlags =
        (SetConfigFlagsFn)dlsym(RTLD_DEFAULT, "_Z14SetConfigFlagsiPPDsP18ICustomConfigFlags");
    if (!setFlags)
    {
        fprintf(stderr, "warning: SetConfigFlags not found; engine flags ignored\n");
        return;
    }
    uint16_t** wargv = (uint16_t**)calloc((size_t)n, sizeof(uint16_t*));
    for (int i = 0; i < n; i++)
    {
        size_t len = strlen(flagArgs[i]);
        wargv[i] = (uint16_t*)calloc(len + 1, sizeof(uint16_t));
        for (size_t j = 0; j < len; j++) wargv[i][j] = (uint16_t)(unsigned char)flagArgs[i][j];
    }
    if (setFlags(n, wargv, nullptr) != 0)
    {
        fprintf(stderr, "warning: SetConfigFlags rejected argv\n");
    }
    for (int i = 0; i < n; i++) free(wargv[i]);
    free(wargv);
}


static const char* kScript =
    "(function(){ var s = 0; for (var i = 0; i < 100; i++) s += i;"
    " return 'CACHE_OK:' + s; })()";

static bool CHAKRA_CALLBACK LoadScriptSource(JsSourceContext ctx, JsValueRef* value,
                                             JsParseScriptAttributes* attrs)
{
    (void)ctx;
    *attrs = JsParseScriptAttributeNone;
    return JsCreateExternalArrayBuffer((void*)kScript,
                                       (unsigned int)strlen(kScript),
                                       nullptr, nullptr, value) == JsNoError;
}


#define CHECK(e, msg)                                                     \
    do {                                                                  \
        JsErrorCode _e = (e);                                             \
        if (_e != JsNoError) {                                            \
            printf("%s: JsErrorCode %d\n", (msg), (int)_e);               \
            return 1;                                                     \
        }                                                                 \
    } while (0)

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s serialize|run <file> [engine flags...]\n", argv[0]);
        return 2;
    }

    ForwardFlagsToEngine(argc, argv);

    JsRuntimeHandle rt;
    JsContextRef ctx;
    CHECK(JsCreateRuntime(JsRuntimeAttributeNone, nullptr, &rt), "create runtime");
    CHECK(JsCreateContext(rt, &ctx), "create context");
    CHECK(JsSetCurrentContext(ctx), "set context");

    JsValueRef fname;
    CHECK(JsCreateString("b5-guid-demo", strlen("b5-guid-demo"), &fname), "url string");

    if (strcmp(argv[1], "serialize") == 0)
    {
        JsValueRef src, buf;
        CHECK(JsCreateExternalArrayBuffer((void*)kScript, (unsigned int)strlen(kScript),
                                          nullptr, nullptr, &src), "script buffer");
        CHECK(JsSerialize(src, &buf, JsParseScriptAttributeNone), "serialize");
        ChakraBytePtr bytes = nullptr;
        unsigned int len = 0;
        CHECK(JsGetArrayBufferStorage(buf, &bytes, &len), "storage");
        FILE* f = fopen(argv[2], "wb");
        if (!f) { perror("open"); return 3; }
        fwrite(bytes, 1, len, f);
        fclose(f);
        printf("SERIALIZED %u bytes\n", len);
        return 0;
    }

    if (strcmp(argv[1], "run") == 0)
    {
        FILE* f = fopen(argv[2], "rb");
        if (!f) { perror("open"); return 3; }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char* raw = (unsigned char*)malloc((size_t)len);
        if (fread(raw, 1, (size_t)len, f) != (size_t)len) return 4;
        fclose(f);

        JsValueRef buf, result = nullptr;
        CHECK(JsCreateExternalArrayBuffer(raw, (unsigned int)len, nullptr, nullptr, &buf),
              "cache buffer");
        JsErrorCode e = JsRunSerialized(buf, LoadScriptSource, (JsSourceContext)0, fname, &result);
        if (e != JsNoError)
        {
            printf("RUN_REJECTED JsErrorCode=%d (JsErrorBadSerializedScript=%d)\n",
                   (int)e, (int)JsErrorBadSerializedScript);
            return 1;
        }

        JsValueRef js;
        CHECK(JsConvertValueToString(result, &js), "convert");
        char out[64] = { 0 };
        size_t n = 0;
        CHECK(JsCopyString(js, out, 63, &n), "copy");
        out[n < 63 ? n : 63] = 0;
        printf("RUN_OK %s\n", out);
        return 0;
    }

    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 2;
}
