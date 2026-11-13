# Vendored test assets (N0.5) — manifest

Provenance and verification record for the test assets vendored by plan
task N0.5 (`doc/node-api-plan.md` §6, card N0.5; decisions PR-2/PR-11).
The matrix runner is `test/nodeapi/run_matrix.py`; the header-only shim
sources it needs live in `test/nodeapi/shims/` (ours, not upstream).

## Origins

| Asset set | Origin | Node version |
|---|---|---|
| `js-native-api/` (native sources, JS runners, `binding.gyp`, `common.h`/`common-inl.h`/`entry_point.h`, `testcfg.py`, `LICENSE`) | nodejs/node commit `fdfa0ff0dbaf0fbf4d7d6d89a2ab807f3177fa5c` (tag `v22.22.3`), `https://github.com/nodejs/node/tree/v22.22.3/test/js-native-api` | 22.22.3 (same pinned release as `include/nodeapi/`) |
| `examples/` (`napi/` variants only, original `src/` paths preserved, plus `LICENSE.md`) | nodejs/node-addon-examples commit `1e4f284f30ed0ac40ec216bdf93ab12f726039a3`, `https://github.com/nodejs/node-addon-examples` | n/a (C-API examples) |
| `node-addon-api/` (N5.3 subset only, §below: `napi.h`/`napi-inl.h` + `test/common` + selected `test/*.cc/*.js`, plus `LICENSE.md`, `common.gypi`, `test/binding.gyp`) | nodejs/node-addon-api tag `v8.9.2` commit `2faf3294972dfcb3707fd5e28d451485f19e3dad`, `https://github.com/nodejs/node-addon-api` | n/a (C++ wrapper; pinned for `napi.h`) |

File counts: `js-native-api/` = 144 files (143 upstream files +
`LICENSE`); `examples/` = 66 files (65 `napi/` files + `LICENSE.md`);
`node-addon-api/` = 71 files (N5.3 selected subset, 67 upstream +
`LICENSE.md` included). Total listed files: 281. Files under this
directory were copied verbatim (no edits); every listed file has a per-file SHA-256 below and can be
re-verified offline (no network).

```
test/nodeapi/vendor/
  MANIFEST.md
  js-native-api/   ... Node v22.22.3 test/js-native-api (verbatim)
  examples/src/... ... node-addon-examples napi/ variants (verbatim paths)
```

## SHA-256 table (paths relative to `test/nodeapi/vendor/`)

| File | SHA-256 | Origin |
|---|---|---|
| `examples/LICENSE.md` | `0fb9453924bf7b66b8ea5a4eebbaf52853efc4efec13582ff1062b1d890eb0e1` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/1_hello_world/napi/binding.gyp` | `f10b269cb793a00bab43b6cdd8100e27d596d17a3d6c0cc478fe225682fbacef` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/1_hello_world/napi/hello.c` | `34ae65f84d15c0ddadf8dfabb9ca6b38e6e1a35c1cab50d709f4c0cbb2f7995f` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/1_hello_world/napi/hello.js` | `ca66e461fdc487e19e64848d1f9a7f89fcbf8bac734f8567da0e296aa8c4a807` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/1_hello_world/napi/package.json` | `0a29cc39504b839e0bb03ef9a1394607fbd0e350b2bad6bfb327b260b1eaa9a8` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/2_function_arguments/napi/addon.c` | `2555cccdd0f97555b1da5fd2e9a7fc36c45a317c60d61293f0de953a762dbf2d` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/2_function_arguments/napi/addon.js` | `03b1b390e3aeb2ff271c4615a4d934e596d24a4facd1727fe76988ce24c9318a` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/2_function_arguments/napi/binding.gyp` | `fcecbeeed02d2b06ed058aaa5e5a02217eebd066b9e8a58b430c60f83254c232` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/2_function_arguments/napi/package.json` | `44f18309d5e1296eb010d1264434354e6f9c71febc33a237409604037c5d6c4e` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/3_callbacks/napi/addon.c` | `7e6b257075c13a917017a6dd1ce3d61808f0623d606887b638e1626d1a4b99d0` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/3_callbacks/napi/addon.js` | `60faa15ec54154d677a22750e7d8af46d835b5bfa8291e7c75a337cdbc186ed5` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/3_callbacks/napi/binding.gyp` | `fcecbeeed02d2b06ed058aaa5e5a02217eebd066b9e8a58b430c60f83254c232` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/3_callbacks/napi/package.json` | `97e530b61adef09a970ba8b4d383a0123ba99245a649514a8ca570e828357ad1` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/4_object_factory/napi/addon.c` | `dcbe9f0afdaaf825bd30579bc7b5a202879316d8864aa7e50e37b54481f953de` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/4_object_factory/napi/addon.js` | `d6fea8c061ff24c176b5a1746596fa631ef46a3f30aff4babbdd88899d9d852f` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/4_object_factory/napi/binding.gyp` | `fcecbeeed02d2b06ed058aaa5e5a02217eebd066b9e8a58b430c60f83254c232` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/4_object_factory/napi/package.json` | `b63f03eaebe9e550bc63cb5ab3a5df06cc0b6b9a22c9c9214387d515e5670d0e` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/5_function_factory/napi/addon.c` | `d8ac82ab67edaf5e00316bb523dfae2416d4b94c436cdd692b7700787a8bfd84` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/5_function_factory/napi/addon.js` | `47a82b896f994f4305c362d9f0fe3dc292fece27ab05fc9d5f37c2dd3467441d` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/5_function_factory/napi/binding.gyp` | `fcecbeeed02d2b06ed058aaa5e5a02217eebd066b9e8a58b430c60f83254c232` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/5_function_factory/napi/package.json` | `c385abcf6ac2d195b50ea0b3847f6095ecf772cc8152596df24d62652ff74eed` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/6_object_wrap/napi/addon.cc` | `98d8862cdb1af3720e0924cfab08342e4e9816257eab8c0853a4f38488b82b5e` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/6_object_wrap/napi/addon.js` | `da543425a62445fb2cb5878626a4885c8eccb82765a34875bd2ce4c0059c9f5f` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/6_object_wrap/napi/binding.gyp` | `923269d66eba856e071b4643226a0b7518e88fdcc2142d2b4e50e55e85f072af` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/6_object_wrap/napi/myobject.cc` | `434d3b523729bc148fb3e9c1a678587a82a0e44ea50eb8ed4f6ee9a6fdadf4cd` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/6_object_wrap/napi/myobject.h` | `49b241d6af73ec3f493f75ab91ec193d60ea0df8cb5c1634752523ef9c6f0d85` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/6_object_wrap/napi/package.json` | `710286a72087edbd2fba2d0dfd7758271c77da071be8f8cb09737aac07b802f4` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/7_factory_wrap/napi/addon.cc` | `b13bd7629b5d6e068e3fae86d02900e0fdacb06e2eaee1c703e63e41a2430c88` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/7_factory_wrap/napi/addon.js` | `d6d56825997129eb253371c1d670f2c9721a2129788b8b8298c41addf70afc67` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/7_factory_wrap/napi/binding.gyp` | `923269d66eba856e071b4643226a0b7518e88fdcc2142d2b4e50e55e85f072af` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/7_factory_wrap/napi/myobject.cc` | `605dfcb36e9eed1f75aa651f4166d9318cd14a583a6ccb0892bc59d1feb891fe` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/7_factory_wrap/napi/myobject.h` | `08237e87e4497f717b3483d9c09c103b7bb2a6fdf1362561f25f0314ba0e7089` | examples@1e4f284 (node-addon-examples) |
| `examples/src/1-getting-started/7_factory_wrap/napi/package.json` | `3464e6991fe4d8bccf1c57997081749379462beea6de8032c3b6af20eb8fc4ba` | examples@1e4f284 (node-addon-examples) |
| `examples/src/2-js-to-native-conversion/8_passing_wrapped/napi/addon.cc` | `490b9261c7f26dbc024d25c746adb91d6b5804170045aad634bd2d5d74ab6432` | examples@1e4f284 (node-addon-examples) |
| `examples/src/2-js-to-native-conversion/8_passing_wrapped/napi/addon.js` | `de1d3f5dc12f3016fe7f1976110fcb0febff1de50c82abac0809f83d11cf0206` | examples@1e4f284 (node-addon-examples) |
| `examples/src/2-js-to-native-conversion/8_passing_wrapped/napi/binding.gyp` | `923269d66eba856e071b4643226a0b7518e88fdcc2142d2b4e50e55e85f072af` | examples@1e4f284 (node-addon-examples) |
| `examples/src/2-js-to-native-conversion/8_passing_wrapped/napi/myobject.cc` | `9dd813a373d25008c85fa01e1aeb05101c66c6ca78d302c6ef67be0217f2e29e` | examples@1e4f284 (node-addon-examples) |
| `examples/src/2-js-to-native-conversion/8_passing_wrapped/napi/myobject.h` | `3a0894c6694ec795b28d40669e34ffd4a6594b0658889aab0386e39ef3f2648b` | examples@1e4f284 (node-addon-examples) |
| `examples/src/2-js-to-native-conversion/8_passing_wrapped/napi/package.json` | `2fb77543f4499d826c79bd63e2cc6a11d148e3b8b1947a553035cbc9da4e90f3` | examples@1e4f284 (node-addon-examples) |
| `examples/src/2-js-to-native-conversion/object-template-demo/napi/binding.gyp` | `b41b5644fff3a37ef98a007ab2f9285567d3f80c0e27cb4fd50b667206ba061c` | examples@1e4f284 (node-addon-examples) |
| `examples/src/2-js-to-native-conversion/object-template-demo/napi/index.js` | `7d0722104dce31281bbe6ded04bc077a583db3dbe18da924e36d4ad39ed691bd` | examples@1e4f284 (node-addon-examples) |
| `examples/src/2-js-to-native-conversion/object-template-demo/napi/node-api-common.h` | `da22c1038232c4c25157e447e49ee1b9773e54693ca7946a553256f3ebd7481a` | examples@1e4f284 (node-addon-examples) |
| `examples/src/2-js-to-native-conversion/object-template-demo/napi/object-template-demo.cc` | `a290aff9b61b5bdd936ebbe37e426f6f559614f42b124703905d6b561a98b857` | examples@1e4f284 (node-addon-examples) |
| `examples/src/2-js-to-native-conversion/object-template-demo/napi/package.json` | `ac0a7b0e6df23f0fc5e625ffdb69f3c07c90076b6775e43da00800d3b5eec7fc` | examples@1e4f284 (node-addon-examples) |
| `examples/src/2-js-to-native-conversion/object-template-demo/napi/proxy-template.cc` | `83591cab09a799330c33d80b523e0f57750e2d04b478c0f042e815b1db57f181` | examples@1e4f284 (node-addon-examples) |
| `examples/src/2-js-to-native-conversion/object-template-demo/napi/proxy-template.h` | `4e0fc1f2717452a85ed96db5c33e4ae43c678d6757ee7442953856eb798b3767` | examples@1e4f284 (node-addon-examples) |
| `examples/src/3-context-awareness/napi/binding.gyp` | `5c0a1a4c4d6f768d70a48990d3e2ca3a0fff5be25d27976cc09e638e8e509593` | examples@1e4f284 (node-addon-examples) |
| `examples/src/3-context-awareness/napi/index.js` | `fe02cd14318c5140609b9120d79d0614014e2609845262bcfde14c4ad18124e0` | examples@1e4f284 (node-addon-examples) |
| `examples/src/3-context-awareness/napi/multiple_load.c` | `2569bcfa85420d9b958d0a03c5374aa02b574e78856902ad612e850e0db297b9` | examples@1e4f284 (node-addon-examples) |
| `examples/src/3-context-awareness/napi/package.json` | `07fded16dd794cb2caf50da8d1c463006c0925e062628550f0e93cd8be969001` | examples@1e4f284 (node-addon-examples) |
| `examples/src/5-async-work/async_work_promise/napi/binding.c` | `1bf87d79d002f6ffd390eeb61d3996ec2b1b9b4699a2c7ceb9e878b8e22a8d67` | examples@1e4f284 (node-addon-examples) |
| `examples/src/5-async-work/async_work_promise/napi/binding.gyp` | `bfa64c13497b91e1b84ab18f19f216fc1b11f900af3a2d2593b4a4325489e34d` | examples@1e4f284 (node-addon-examples) |
| `examples/src/5-async-work/async_work_promise/napi/index.js` | `025909fd2e36aa3f8a32ba19736c9b4860da337476905375d9c7562a018b3243` | examples@1e4f284 (node-addon-examples) |
| `examples/src/5-async-work/async_work_promise/napi/package.json` | `160a1435d962dc082da0a74d0655a6dcdb5ed79567419e16712ba463505bb708` | examples@1e4f284 (node-addon-examples) |
| `examples/src/5-async-work/async_work_thread_safe_function/napi/binding.c` | `f57b76f2ead982aa49c9e4d297a18165f6305773145069e0b6e66188329d47e7` | examples@1e4f284 (node-addon-examples) |
| `examples/src/5-async-work/async_work_thread_safe_function/napi/binding.gyp` | `bfa64c13497b91e1b84ab18f19f216fc1b11f900af3a2d2593b4a4325489e34d` | examples@1e4f284 (node-addon-examples) |
| `examples/src/5-async-work/async_work_thread_safe_function/napi/index.js` | `1f5d7ca0775698bcce40d4047a8bf07efedb084518700dacf00bcfa00266fcce` | examples@1e4f284 (node-addon-examples) |
| `examples/src/5-async-work/async_work_thread_safe_function/napi/package.json` | `8e261c306fbb3133286b2c9dda745ff8c6d5f8c4da6aac3eea9aaf518eeb88ab` | examples@1e4f284 (node-addon-examples) |
| `examples/src/6-threadsafe-function/thread_safe_function_round_trip/napi/binding.gyp` | `fc0bb957f80855b75a23025c0c983bff74e0abb10b66afadfb0c3cdc5c031931` | examples@1e4f284 (node-addon-examples) |
| `examples/src/6-threadsafe-function/thread_safe_function_round_trip/napi/index.js` | `4b871e4a87c13ba4ebfba2c137e819ce730db5cca66c87c68771a0532ff05de5` | examples@1e4f284 (node-addon-examples) |
| `examples/src/6-threadsafe-function/thread_safe_function_round_trip/napi/package.json` | `174c8d86514b6789cd2d34cfb3c0e00c15899fd245ea0df7b394610210bcaea5` | examples@1e4f284 (node-addon-examples) |
| `examples/src/6-threadsafe-function/thread_safe_function_round_trip/napi/round_trip.c` | `29baeadd64452126c9758dbfda1337835fae26647325f4b0b3139370bd55273e` | examples@1e4f284 (node-addon-examples) |
| `examples/src/8-tooling/build_with_cmake/napi/CMakeLists.txt` | `4ab24b339c37de2d7fe7c805f4fcb195f944a7e9d91d990d69d114bf0be19b38` | examples@1e4f284 (node-addon-examples) |
| `examples/src/8-tooling/build_with_cmake/napi/hello.c` | `7bd2916e46d09a3c8f43a67c61c7b2e2e97dc83b9c233e72c6cfcb96f616bfd5` | examples@1e4f284 (node-addon-examples) |
| `examples/src/8-tooling/build_with_cmake/napi/hello.js` | `ecc6faac5371f3e500cfe1abe4290c20ba1f86303cf7a24c0aec9f18381e6db0` | examples@1e4f284 (node-addon-examples) |
| `examples/src/8-tooling/build_with_cmake/napi/package.json` | `69e4162eccbb38b5a6537fbee75e5d41b8343409e1de29309b6ae62aaae54d15` | examples@1e4f284 (node-addon-examples) |
| `js-native-api/.gitignore` | `27074bba2b4ff8badebc2e75dcf7d513ddd12a91da7b665772d6f4a6a42e817f` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/2_function_arguments/2_function_arguments.c` | `8ce7be221d838ef67fd491437916130a60d02c92a3f75489e0c396730fa8041d` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/2_function_arguments/binding.gyp` | `f47faaff1d45b1b7a25236709762cffd22af1cabfd345020e489f7926759d674` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/2_function_arguments/test.js` | `07fe2dbbf28926ceeabc301f756708537eddc3a50fc619101d96c77723082f30` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/3_callbacks/3_callbacks.c` | `9fc32111d044d31bd92a593a56a14f3af9576c0ea0eaceb57ce16a20976ef9e0` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/3_callbacks/binding.gyp` | `66cb54a2ab54876831eb8561637a68b50645115596739fd415b4d8c8dbd39660` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/3_callbacks/test.js` | `8c5d1b3b5caade9400e7d7db3ae8671b65b667c9260c7392d40bafffb56a84cc` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/4_object_factory/4_object_factory.c` | `4c79fd2bffc3a8bf180d3508a27ce1feb4ab8765038f00d08ba9555bfd4851d7` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/4_object_factory/binding.gyp` | `094b31d264ee522aded4b87e0f4ab8c17bab0965437b640a95f34e3d0d398ee6` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/4_object_factory/test.js` | `8de5b177f88c709e88ee2a092abb8d0beaa25b9d27c53da5410ba4799e8b096a` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/5_function_factory/5_function_factory.c` | `b6bb82796b74ed399bca62b65d55da5be36417a70dc5f497454dec60854df6e7` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/5_function_factory/binding.gyp` | `0808fe6890904da82d0956e84541f32fe8d0a676c781610d76820bcd8c766b97` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/5_function_factory/test.js` | `e377215b3728dbe4845eeb99158d97819688af2812b9924eeae63ed88769b627` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/6_object_wrap/binding.gyp` | `098afb39291fc4aebd373fd3ed0a9df68edcad1930eb577e1bf7632427371e0a` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/6_object_wrap/myobject.cc` | `956e7335da2a8b88beec2cc22cb0451604ee4698f0c6192431b41c46f61eecee` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/6_object_wrap/myobject.h` | `ab0e131bb0f750087a8f0efb737215904b63e64fdeaafdcb88cbe4a824fe5140` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/6_object_wrap/nested_wrap.cc` | `f49a6f6c01b066a2e664a7a45c27070c8221498e38f24ab7b8a4860ab0e07f7b` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/6_object_wrap/nested_wrap.h` | `e1b62ddfae8ac5c1148b968206c73191d708c9f83cccab278f060026c588ed0c` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/6_object_wrap/nested_wrap.js` | `e6ee38ced2cd1ad1dc5db7b05418d5bf6f0564b531bb9f6075e70101a3901478` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/6_object_wrap/test-basic-finalizer.js` | `33e1b4f8a5568f126d34ca30af10cc96ee861744bddef52bea881422e120b91b` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/6_object_wrap/test-object-wrap-ref.js` | `b67d420d0321a4f4df4455f32d854189065fc9507eefb8b67871622484804383` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/6_object_wrap/test.js` | `81b2a17302d1c913151c738ed80326a62bbf9cb8c20dca79b34b39bc4baf2b1f` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/7_factory_wrap/7_factory_wrap.cc` | `e07e195231a9a807ad39e775240136ce44aea3929200cad5bbe9e99fcbbf8e00` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/7_factory_wrap/binding.gyp` | `15b728c21648a6b3831986bd6bfeed1ba7a5c16bd07bb3d30d2405a7d50d2bca` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/7_factory_wrap/myobject.cc` | `d977e7270b9fceaf9b5307241d4825bf2176d968d4414453af6724da4be6592d` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/7_factory_wrap/myobject.h` | `8f5154f31fde8aa563ec7c70d506364810f9a22061ed80194dbf82da9c055a1d` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/7_factory_wrap/test.js` | `5c8fbb9f6a32f9e3838ecbdc49aea5e6063970fb31d37fe447b82dbbbcdd0a03` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/8_passing_wrapped/8_passing_wrapped.cc` | `9eb1a132c33f9346a991abc5c0304b1b5fc8854a4736f48e6340b79d0c92c4ca` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/8_passing_wrapped/binding.gyp` | `540c980583157d052dd5ce0735f28dc1365735085f0de1d47c5e6d21767d93c4` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/8_passing_wrapped/myobject.cc` | `4fa4c7a130060c5656ccff2cea59684bf5d7cf4c0a1e2a6f839ecdeecc4433da` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/8_passing_wrapped/myobject.h` | `935b95b281326a11971681250aafc328b0c704cc0d82e2dcf9316fdbcb2be6c6` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/8_passing_wrapped/test.js` | `9a9389285334d29bb8967f9ba52de0569d4ce4148c7fa9fba44e8651a8540fb1` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/LICENSE` | `c738ae413cf561f174e34f6961f8ca458aae2369a73640dda6234c629b98bcc4` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/common-inl.h` | `cf88560187040541c9e144974f92247c7de093980495faff75487426a741dfd7` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/common.h` | `98404bfb5411054608cc0516e46585331233dd31714336ed33538b1842ca6344` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/entry_point.h` | `4a679af218acb2cad7f602d5c1c793b1fdd5b3fdf2e89907c4f904c0b5d999dc` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_array/binding.gyp` | `55b09acff909f311e8a19aa1ec7311ecf2850b282690d0a585a1590f92917cb3` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_array/test.js` | `00728a00b6bc665e53a3ac989092ae4f5a683b2f1e7780f0f979e3b40137f851` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_array/test_array.c` | `4dd6e92c9b0a40d71efd2356aa37dc0796e7c8956d85d13283673b4ff05447ad` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_bigint/binding.gyp` | `5774e5788e6049f3a40565506c7e3b67d5dc68dd3886b5b893088cac0e6ee866` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_bigint/test.js` | `8cf8c21fd5c131f3c70c7c3c27c1e2d0557b5fb74d1349904710c333eaed13c7` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_bigint/test_bigint.c` | `a8a21a60801e71fd4ee891095685e4851a25d70453ba78043607294d46a54ddc` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_cannot_run_js/binding.gyp` | `7e03258f341386baa19ef27ffa7324a8b763e4749f9d1fafa26cc2ef66b63e1c` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_cannot_run_js/test.js` | `4927101369599ba21dbf3ce6a53d5f3032744f55a0143b4f324e182b2e4fc76b` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_cannot_run_js/test_cannot_run_js.c` | `f02b9a82103fdd203fec06aec84a90c927f4090bce4369d16d7267d12015b539` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_constructor/binding.gyp` | `5285c1e9ae6e10698e5ecdb729171fbd542965214aae58cdb334157a7ea5fe78` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_constructor/test.js` | `aac572faf7b07da05d3da1193a3702a165fbe36047955181bd25f6537b290531` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_constructor/test2.js` | `affba5d1c41e2e8eea6832f3138b97a90b77ff70e87d38caaf6ca871fdabce34` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_constructor/test_constructor.c` | `f7f6e7a9021978919a8b984e0861e74b2186ac3041320deaaa0f7ee7ba74de5d` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_constructor/test_null.c` | `cd9380d4d8da8e254e96389444c38add7ed8d74ea40d500d8f47d7c6d380fb64` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_constructor/test_null.h` | `a839c5286afcbc812649bf2b705fe5393f90720e8768b998abb686ad65b5367a` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_constructor/test_null.js` | `f97faf78eeb05ead71ea30e7579a9cfb504012bfbef63de4adfacdb5c6a211c9` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_conversions/binding.gyp` | `70ed581161260886f4b217c4646cac774dbe26715a709444d48ba3423d974410` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_conversions/test.js` | `0f8b9ef25497ae6e18430d8fcc86d2e7f5d807288be1f15a504858c8c4cc1181` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_conversions/test_conversions.c` | `c669f2840b9132b8ee7846bfa71e923156ec0e180f3a150cebe00351586a85d2` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_conversions/test_null.c` | `3051e0ef7ab0db74d6639aaf2cc27bf5c965970dafa00d9dc0544fd3364cae46` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_conversions/test_null.h` | `6ec86f948694ef968214e5468e6d93e4919128c7cecdf9ff8d37ac1e5e759da3` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_dataview/binding.gyp` | `e97faf5229f359c8ca18c38c0e3e613c90d47bcaaaab47b13a90e647a45a27b5` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_dataview/test.js` | `e5f0116a03c40c394ad6972bcbb9bce7ecb603b0f9ae07de1517ec0e9f7ea17d` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_dataview/test_dataview.c` | `5722b3ff7f6d0bb8334afdc385a3a3f2b237c474bdb611f5d98d9b391d70d186` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_date/binding.gyp` | `67ee9cbbd0d349753f7a537cc63b04964015394137e5c653ae831c7b7f2dc42d` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_date/test.js` | `a243309079271a5d62a47ac5704d22763539412fb7a5a08251d0ff417c3c78a9` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_date/test_date.c` | `7c58bd5b7736d43b7223a967cd13833e3edd0984e303d1c262dd84c24e00d9f6` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_error/binding.gyp` | `8495ee2691e0a719daaa5749f12b528bca9d56b019fb3ce01becdde54159f82e` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_error/test.js` | `d952cadc8c73bcee0f514f31b7081b89dc0dd96b5de2ca17b99da92c8d5245e8` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_error/test_error.c` | `19198d705def231ba590e27a0bcabf3ef14b52066fb5c415b5f251fd95fe2408` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_exception/binding.gyp` | `16e04884fd405931648426603be9502675c005c2f2acbff95e4a3d7720172d86` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_exception/test.js` | `5c7b833a2b3f9e4b44a292ee2105adff539e51676cbcf750fa3c194a99cfd85d` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_exception/testFinalizerException.js` | `a8ded426134a8bbc1bcfb08f5aa4ca8db099a474c725c66d312a03f6e145bcf9` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_exception/test_exception.c` | `a70bdda952dc0add0b95ae89352d11373dbbbc4fc29901bdda863814c4861417` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_finalizer/binding.gyp` | `fbba0218807f908159747e231413fe6be5fabbdee42b802a643380777b581967` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_finalizer/test.js` | `53c6ea643b7114a2f5bb2afb538918e5ceb0bb4528aa093dc9040fd6f233b60a` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_finalizer/test_fatal_finalize.js` | `d73645d359354865629bb7bd623ed0bd10dbaa98c5d0c12a3d25d5f151e92c1c` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_finalizer/test_finalizer.c` | `cc260cae0bc877402563121f4c0e696097d8f52843baddc0f7cdce7cd183df5f` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_function/binding.gyp` | `9eaf8c62ecead0cc57934452f491ec6ed13c40390a20220c2311c5a1ac75e021` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_function/test.js` | `283b0657456b144be9c592a00026f3406d1896cd90518048e892bf60b8416574` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_function/test_function.c` | `267886a51df858e7665dbc51b0aab4dda88784f28561693986089f0c360fce5d` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_general/binding.gyp` | `2426b5f15a42bd759c38e3c45169c5dc12a1dc75286f7139c8c866f4d47877d9` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_general/test.js` | `f5a7307f985eba4d8a6eee1387d09642fc9ecc601e08bee3caabd67e06e702d8` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_general/testEnvCleanup.js` | `58ebb9fb2cae2645285efabccfe05b4ea350da4f4f8b0486116837e9c3c3e691` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_general/testFinalizer.js` | `3041c61e529a6639c12cd09483aa637dc3bcba84567034328b80eadbc144b24f` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_general/testGlobals.js` | `93db050c00bd0bc577920742d70c4fdc801a6788014f7ba0200caab2d65d8521` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_general/testInstanceOf.js` | `632fc632a7d27af6de1606d293bf474fa12556835791ac2d9d72317de3e0cf5e` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_general/testNapiRun.js` | `e1d63f924e46251e763f26d711087ae45707f7bf35138803e9142dc8c3358959` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_general/testNapiStatus.js` | `1b3943b3e4cebac780f05db766817da3d24ea5f8a6216ce71efcd58b3f015b94` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_general/testV8Instanceof.js` | `6e9fab6c0b6b5c0dfa39d247a7921fc1f41284ef0bc18ddcf864086066c9de48` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_general/testV8Instanceof2.js` | `9450afcacebaeb8ddba8c7083637f43e30336d4af0bd2f3f3dad21d7cb40be34` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_general/test_general.c` | `0fe004afd50a798268e9d2322c29861a2c75dbce25f71fdb6f725814b78f6845` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_handle_scope/binding.gyp` | `132bdfa67ef4a46f2d6cf7250f5ead4261bbb766211539902bb5b2c93d2ef62e` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_handle_scope/test.js` | `6300d0974eded93fd1855b461c2ea38a69de90e180c8a0ac2e6f64e087818e12` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_handle_scope/test_handle_scope.c` | `ee1186f7576d66a1fd2f3bd1f7650d14fa6adfa2d1c02e1d60bdacc454dfe02b` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_instance_data/binding.gyp` | `634a95269811d600344b983f3dfc58c20632450e6dd4b2d84977414cb072dfdb` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_instance_data/test.js` | `118d7d81f157640c7828f022218bdac5cc6b0ddecea6cb0ccd674fd5e672e42a` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_instance_data/test_instance_data.c` | `b51636b968da67bf05be6cd3f9ece83e75a110e071265a080544f1fcc042447d` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_new_target/binding.gyp` | `33b9e22b1b148de5d6828114e797cd31831fe5988b68d27ccc46bba6149b944e` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_new_target/test.js` | `1fe6525a4c323cb8bc9aeeb04628b1f6b6bc169f43a4c56d08b38c7b908daff7` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_new_target/test_new_target.c` | `748519dc1067bffdf731b5545bddd3a4689f58539a39edcdb1c808a08f212caa` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_number/binding.gyp` | `5797bece329c1bb2df2af0ecca7b5b41fef7765676eceaca540c10a134f4bb70` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_number/test.js` | `3691cba5fbd5723fea57a0c4c76042876a722dc31323f32aaa211612d8f625bf` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_number/test_null.c` | `20fac9844ca862da0e6a7d853f58025b455445e4f8e2df807e6a341425bfa30f` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_number/test_null.h` | `2d19ddca8f62a48e4db9df23f2e62b2b51d051459fdc8eb27413f05be26b52d6` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_number/test_null.js` | `1b0635c17e831abb3d3b95f36c12c8d7c40793d14f45e2bf76ae84ba33d3f234` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_number/test_number.c` | `6e473a1d9b8c60ba9a9e369d8570c88b9dcc4e7e7b328778fcad4e959c7a840e` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_object/binding.gyp` | `e70279838d58d116abf6e5bf96f2c56d89c7e821c4a34c260a2336aaca56233d` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_object/test.js` | `9abce53eea626485eec16228503919ef044735ffdde174aae2ff44480696a9a1` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_object/test_exceptions.c` | `beb239688660625127b9dc48bb03c1a5143409bc0a75a684ba00dea7cad25aec` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_object/test_exceptions.js` | `5225cda74bb8e01c92102cc2236e67c647763da8ed0b85b69903c2060d69ab69` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_object/test_null.c` | `042d1e1ee1fe0a1933f795acab3a1ab64d5c288b7965abdfb194aa13896255e0` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_object/test_null.h` | `a839c5286afcbc812649bf2b705fe5393f90720e8768b998abb686ad65b5367a` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_object/test_null.js` | `d6bdf9a732306970066dc6da0288bdd77e3c25e2b82b13101e36bcc3e2fdd606` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_object/test_object.c` | `aeb471d91b64b3858ce98633702bee13796c8702fefa0eaabf341339c2c1b524` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_promise/binding.gyp` | `672bd70748bfaf3b2df30fd5980697d2c3a6a62a2511eb0ae302ec730820cc42` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_promise/test.js` | `22b8a67d3019937a2fe3210ef4065ee53efc23f310a7f0e12adb56687d66da3a` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_promise/test_promise.c` | `b5f7108be3182e8537e73db1be07f0d3aec2b412c8d6b7e60c3ec29ac55a7623` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_properties/binding.gyp` | `c2a41cdb44a393b80a5fc2b7d403695fefa46a0c74a3351e71805f50d97eeae0` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_properties/test.js` | `9a95fee026c9cf48b0853b88c68a2fea0b8667135d32fef02a009369a54956f9` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_properties/test_properties.c` | `7faf46171e281c90c0e42f6bfe0b282ce9679b944c913a0453541cdad766be5a` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_reference/binding.gyp` | `a777c42e4810dd5c84646a28043c6d8b5db15a02e79c8273130e64b49c05aeee` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_reference/test.js` | `3fd5886d2ad7d60f42f728d13e3073b3009857c9052804cf28decd4c50796c69` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_reference/test_finalizer.c` | `b4b88e87a99b5e3f45b4547a6c228fbd10ebabebed40c2d32cc471e9a8045e92` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_reference/test_finalizer.js` | `9e384f72938bd81c19f4678e920dca0c94f6a0b8625dba7418fee4056a0cc576` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_reference/test_reference.c` | `ce2a2e881b7b0390ab7e2bb332bd21a7857ed8ba143fc450ad3efd8315e8bdbd` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_reference_double_free/binding.gyp` | `d3ba7151e61391be9b16cb639280455d123ef7994a10f571f5cbf711a10760ee` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_reference_double_free/test.js` | `93fdd5286e70fd16755bf01e81d6417435c9a3e062bc5d505aef006108cffdfd` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_reference_double_free/test_reference_double_free.c` | `9129d9bef4c402398322b7abbaf5ea4ca55cf2043ef80cea6161a7b9c3c3fe76` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_reference_double_free/test_wrap.js` | `178927e81490533deb51238a23873d1e24d03dc4279790695fbbee823c883e51` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_sharedarraybuffer/binding.gyp` | `f972ec0680ceb379411b4c19bd1f98930fddb27010e48e59d87c37801220479f` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_sharedarraybuffer/test.js` | `574a6606e448d0708cde18ecc0504563a8eef6b6acd936323db68d9bf2f2882b` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_sharedarraybuffer/test_sharedarraybuffer.c` | `d8e14c40faa96f26a0a13236962d2369d4aa9f057cc956f7cb81e5c1b77ef8cd` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_string/binding.gyp` | `ac7ffc1ca4cea072416ccaddfd011986db06af533f5afec58900217ce2817aa2` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_string/test.js` | `e138d90f5a661b0e6a118af7afe9e15486d765bc8f3872d31ef0123f7be67e69` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_string/test_null.c` | `704f03ed1b437cf293a5663745c8903d1acbca9f2b235b053e15fed8668bcff4` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_string/test_null.h` | `391627e64cba8535eb61fe919d8df43206ac2814f3964ddac40a00aabc74cec6` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_string/test_null.js` | `202e42d170ba58f05cae67afd01aef170a71fc2631c83912d4c53cf5a184adf5` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_string/test_string.c` | `cb288f80ca16f5b5b29d439292d4eb436706fd12ab58b2093ee4f48edba740e9` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_symbol/binding.gyp` | `f3de57cbd8f4b984b36654213d2c45ccf436c62b348f27edfe0f4aaeef825853` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_symbol/test1.js` | `9bbab6295551d1e4b4e4b688d237183d79d54e6272de3b2768f33f9396d640aa` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_symbol/test2.js` | `db4e7a679da33de35655af9ff782f42ec1a0717263fc3d857d4ab2f5c79798fb` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_symbol/test3.js` | `d96b349e361eed1ec36e16a3f53da17c71ec1d928d08776b165a154b411544aa` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_symbol/test_symbol.c` | `8df6b136a2bef191156e5a05c9bb5037130886b9b7e1501fb794e87270c1bea1` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_typedarray/binding.gyp` | `41f56706f61c72e8ac50e84bdcd6400eb43ada6523a28703107a2ef5414cde81` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_typedarray/test.js` | `9960beaa41351000cd6070adbaaf808b427220e2ad5b56349e5b8346812612f4` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/test_typedarray/test_typedarray.c` | `3f43c32e9a9f8b9e62d92d9681997d580dcd0027ac78651013489e12cb6ebc3c` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `js-native-api/testcfg.py` | `e2a73db3ef655254937c5800913bb98d8f949edfd9833a9da078658c58144472` | node@fdfa0ff (v22.22.3, Node 22.22.3) |
| `node-addon-api/LICENSE.md` | `89024017b88a9f2b763f79b941a4f2db3b4428edfcacdc0b23866b2da633ad0c` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/common.gypi` | `c2588f59f1d4eebe14331b2cfabcfdff85e8c282f9f66028385cffece8aa85e9` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/napi-inl.h` | `83f7520e9028cee918ca876def8cd982ee67906f2b587c60a9d8d79183b881eb` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/napi-inl.deprecated.h` | `b66ed1e565e735bbfabcc2f72d466ab7c5414d3ef8851d1af440b81be7f2375c` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/napi.h` | `5e0eff20229ba021fa7c5ff6f62dfecc36f8c29f2da365b25cde3d6489d8b924` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/basic_types/array.cc` | `66b23ecb0ad0244a9f8096a2555a396bfca05812ecc84c36bb8e250c1b299de9` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/basic_types/array.js` | `4a1eaa7edf34adbd054cd187cdee1e28e8dc27d13eee8b9ac4d7ab28d57a9b8d` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/basic_types/boolean.cc` | `febe41d5b18abb01b9764c373b18a6624438802a4c760ffe11431b28d761e3fb` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/basic_types/boolean.js` | `5315c190f5572d4fc6e3673ca8d0f6f48d06280e706411cf15595199d49ec3d3` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/basic_types/number.cc` | `f6b0594e551360180873cdd44964e77da9def4e9cc409837414ec5ccfa7ad554` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/basic_types/number.js` | `f757625612f0ebab9b11eb850160679fe2447837ab45f43bfaaf50b387b7ed35` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/basic_types/value.cc` | `ae1aaecf0a2b4269e4914c45381a73c6c706f2aebaea68da5c688a2a592bbece` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/basic_types/value.js` | `ffd219782cbf547056aeacf065b0421f3510d4cdf785d3df83058a0a76459dca` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/bigint.cc` | `d8cd362550c6b7ab5f50018b48b1e946843612c97c62ad90ed134919acef8739` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/bigint.js` | `041725648e876bafa087911528e7bb09235781aa3f2f80d27fc57781ba449f49` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/binding.gyp` | `3e0f311650d5d47f9c0ea2853659e5702cb41367b56f342de8983d01aa584c6c` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/callbackInfo.cc` | `78a868bbc5d2b9b1fc4d94035095d7961543e784fb44266ab2b1340a7fbc3716` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/callbackInfo.js` | `a7975ff4e6f10122cfece21ae34f817d0fdf662ee709a80ef6a4265d852291a8` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/common/index.js` | `349d032555b193a88b3ea91780794b2651caca095fca85350f132ebcd3cc80b9` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/common/test_helper.h` | `8dbdc734e9573c4ce37ca1212cb24d695c4789dbf174c4f11bb0e5a3e28547d9` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/dataview/dataview.cc` | `feb6af2894dd486bb93899f29f7b1b1fdb71f2b06bf9dde400ed44c6364331eb` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/dataview/dataview.js` | `82e35668613de97d1c08ea9cea46a58a92bd5247159c160b80fcffb25542ceef` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/dataview/dataview_read_write.cc` | `218089409a911bab0b311fe2e3007261f2684617e7384927a5b43b3a0095eba4` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/dataview/dataview_read_write.js` | `65eefb89205eaa5f59e93b74ead8383c79211ed99dcf042d6d28e4d20ce4c253` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/date.cc` | `ab3208598c0d9865c00e07c237f9aa4da553013f58f3dc914b79851c5c162091` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/date.js` | `0b09f3364b581d40defccf4a9a37e099e0b3ed05a02365a243cab14a0d7b270d` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/function.cc` | `9c76c9e05a0be722a1ecba220f4c3668827a28c0cb8e7d6c8da31b1d36914fc5` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/function.js` | `7f4f78a7584cfa4acbd30eafac40d3e07eb878e2808c196ef82e2fd50688c50d` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/handlescope.cc` | `40f061c036848336c14fbd29b3592f0feefe4b8dfbb8901448b43a54972fcea3` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/handlescope.js` | `56b300dd3b6f4de6e54fcd688ddc4bb6dfd0e94dbfd5a774bdee3b11c8c222c1` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/memory_management.cc` | `fea80a701fda8e223efaa0bbf07565a37f323ab17350087000556184c9f06f88` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/memory_management.js` | `798aa3fb59b6d4e5cb1fc03fe7de5ca7f1a51d88024988615107cee520befd41` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/name.cc` | `4b688b67098cdf592702ef967f833b529d5111f8b6021149df612d45518a7ca4` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/name.js` | `695558e45b216fbd5a6093ab9c1527c0b375bbe2ef1a8b123710c67c7d153817` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/delete_property.cc` | `d2b4d7c4a1a8e6eda25d6a897dd5bb5e380ab28469a6cb71f91c44303b638b9d` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/delete_property.js` | `c4c01e294f57d4daa9ac8903402edf6dcec71bf0db0936b1a8ec42621526582f` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/finalizer.cc` | `85380c8a4864e04421101103434d0e506912b01143f66834dec11cef21e356e5` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/get_property.cc` | `f8d52f248c702ceaf9bb08998a940e0d95b3ec5f89ef9d709c96d1076be1ef33` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/get_property.js` | `3e1767c5c0cf0c8a1238104c3565f872bedb8b9d77663c0ee8f9f523788c68d4` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/has_own_property.cc` | `9fd982cba35bf8fb0e4bb572fbb0295d158d14049cdd2d0935b5a1b28cd9da2f` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/has_own_property.js` | `39df0fd13674378718ec176424e8d5aa63b53c80c00a8e9792d455cf6c5af9a6` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/has_property.cc` | `2d05f9ec44db91631247f653e66d8a8d86785f90d9116a9e4fd496ec9b35e1f2` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/has_property.js` | `fdc146e103fc6f6e44712dea47351e9da8b6f4d5dd03b182f903551919c78576` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/object.cc` | `34b7f88e751e53b9a62b9f0e6efb7ebae7f2d3d539ff60dbff41e3d431040690` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/object.js` | `1691d9650a515920bdbc4501daa512d82523c24827090d2499606f910327562f` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/object_deprecated.cc` | `5f0164ef6abdb27c4f2c92007e7826c83d780a4dd1d7575ff78681a8843c2986` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/object_deprecated.js` | `5aea154ed87910741c9d5e4142f83f55ca4411b6b7c9efc8db079c1da8849f7d` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/object_freeze_seal.cc` | `bb5050b84a38e5c630b30f73ab005be3a8efcf1d3e6784d79c2bc7bf1a12009a` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/object_freeze_seal.js` | `dd97330b489aa48bb510a0a42a5f8c1d68d811014e3490bfefb9ea9e39f26021` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/set_property.cc` | `25f5f14e2e97537f5498046ec7002d2465bc6c8334f6d78cbd7a4877404dd99c` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/set_property.js` | `068f9932d66d19ff14c1d203724f53a6ad98c7ed45356428dc5aa885fd956442` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/subscript_operator.cc` | `3f9c2eed01bdfa389f948529421ca767e0050feeb0a606af438a7978dc984f39` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/object/subscript_operator.js` | `fcf241e640abea0915139de277470e40a4fc024469d5ae4b0aa462986e6c87d6` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/objectwrap_multiple_inheritance.cc` | `ecc6639baf6a8c2897ee465df2da3111bbc05d49fed71214a64aef57addc582e` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/objectwrap_multiple_inheritance.js` | `5dd8cda88f8cafc4cb20ff898fd199ed063d0457a29b38e7b9c133084cbfed9f` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/typedarray.cc` | `b3e238e7f757cf312f1469644cf13058e675fef015d10e0e83699494faee4f49` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/typedarray.js` | `cea5430bec4483c9dd44441d8c3c0cc10e88b858ec13b7025e2cf8d7f45804a2` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/globalObject/global_object.cc` | `0dd0497e98d272a87972a5b55cb187a1b90faad6e77c2495ad6464669df6187e` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/globalObject/global_object_delete_property.cc` | `b7e6a0d22e2ddc923817021f1500d03d6be60fe09eafbfb0c5676036b7783faa` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/globalObject/global_object_delete_property.js` | `e388b00f23a8191fc08f73f78c06d81bc99b205f526b1b7c7e7f72c5ebdc4801` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/globalObject/global_object_get_property.cc` | `be0ede4de290e100baca40f8dc374877f4e47d62042399c351c30b0220a8cfa2` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/globalObject/global_object_get_property.js` | `cb5d42c123946dc0131f42e89acddf5c22b013da1ba5804fc05ab885fee7ced9` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/globalObject/global_object_has_own_property.cc` | `d88eb34fd1f5c5ac233fb6e6c60ee50680018f42351287037afd2fa61ae0bcc0` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/globalObject/global_object_has_own_property.js` | `550b26da3230c52c62ea7098540323ebc2b8f8be24448126163937c2526a863a` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/globalObject/global_object_set_property.cc` | `3ce885464c5a57931599382f266dd60904bc41b68cd9129ae972721f5c0318d4` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/globalObject/global_object_set_property.js` | `5d84cf3c09b4cd271947f2937abd8b3199d2e0ce4d018f5a912bc7b48ba55d81` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/error_handling_for_primitives.cc` | `bb1d4df4baa0af2108c1dd4e033252ef938e05bc380d96eba7d79d6caa72b16e` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/error_handling_for_primitives.js` | `f3bc6620bbf0b28119389ea3f0fc7cefc07d41c256bac991ffd2260dba5f316a` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/thunking_manual.cc` | `af77f59b24de38e64b0d092b4dc90ca2ecffec733afe30a96eed4c396ceca725` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/thunking_manual.js` | `ee7bb7edba09f30f8755e27770b36922847c5488256a821109c941c3c18f9071` | node-addon-api@2faf329 (v8.9.2) |
| `node-addon-api/test/typedarray-bigint.js` | `73e5d30d5d4151dc43578d8a95a30aaf4d4616b8ae6932f31be7d53a87e3da59` | node-addon-api@2faf329 (v8.9.2) |

Verify with (exits 0 only when every listed hash matches and no
unlisted vendored file exists):

```sh
python3 test/nodeapi/run_matrix.py --verify-manifest
```

## Runnable subset vs excluded (classification)

The runner classifies every (target, driver) pair up front
(`run_matrix.py --list` is canonical; the tables below were generated
from it verbatim). Result vocabulary (plan N0.5 taxonomy):

- `needs-gc` — driver drives `global.gc()` / `gcUntil`; the test host
  has no `gc()` hook (N0.5 skeleton scope).
- `needs-runtime-global` — driver needs Node runtime builtins
  (`child_process`, `worker_threads`, `require-as`, `process.*`,
  `Buffer`); nothing requiring fs/Buffer/process is in the runnable
  subset.
- `needs-node-api-version>8` — target needs `NAPI_VERSION` 9/10 or
  `NAPI_EXPERIMENTAL` symbols (absent from the v8 provider set).
- `provider-gap` — provider/host surface absent (async-work / TSFN /
  libuv / CMake-only example / unmapped driver).

### Runnable entries (45)

| Test (runner id) | Target | Lang | Driver |
|---|---|---|---|
| `examples/1-getting-started/1_hello_world/napi/hello.js` | `hello` | C | hello.js |
| `examples/1-getting-started/2_function_arguments/napi/addon.js` | `addon` | C | addon.js |
| `examples/1-getting-started/3_callbacks/napi/addon.js` | `addon` | C | addon.js |
| `examples/1-getting-started/4_object_factory/napi/addon.js` | `addon` | C | addon.js |
| `examples/1-getting-started/5_function_factory/napi/addon.js` | `addon` | C | addon.js |
| `examples/1-getting-started/6_object_wrap/napi/addon.js` | `addon` | C++ | addon.js |
| `examples/1-getting-started/7_factory_wrap/napi/addon.js` | `addon` | C++ | addon.js |
| `examples/2-js-to-native-conversion/8_passing_wrapped/napi/addon.js` | `addon` | C++ | addon.js |
| `examples/2-js-to-native-conversion/object-template-demo/napi/index.js` | `object_template_demo` | C++ | index.js |
| `js-native-api/2_function_arguments/test.js` | `2_function_arguments` | C | test.js |
| `js-native-api/3_callbacks/test.js` | `3_callbacks` | C | test.js |
| `js-native-api/4_object_factory/test.js` | `4_object_factory` | C | test.js |
| `js-native-api/5_function_factory/test.js` | `5_function_factory` | C | test.js |
| `js-native-api/6_object_wrap/test.js` | `myobject` | C++ | test.js |
| `js-native-api/test_array/test.js` | `test_array` | C | test.js |
| `js-native-api/test_bigint/test.js` | `test_bigint` | C | test.js |
| `js-native-api/test_constructor/test.js` | `test_constructor` | C | test.js |
| `js-native-api/test_constructor/test2.js` | `test_constructor` | C | test2.js |
| `js-native-api/test_constructor/test_null.js` | `test_constructor` | C | test_null.js |
| `js-native-api/test_conversions/test.js` | `test_conversions` | C | test.js |
| `js-native-api/test_dataview/test.js` | `test_dataview` | C | test.js |
| `js-native-api/test_date/test.js` | `test_date` | C | test.js |
| `js-native-api/test_error/test.js` | `test_error` | C | test.js |
| `js-native-api/test_exception/test.js` | `test_exception` | C | test.js |
| `js-native-api/test_general/testGlobals.js` | `test_general` | C | testGlobals.js |
| `js-native-api/test_general/testInstanceOf.js` | `test_general` | C | testInstanceOf.js |
| `js-native-api/test_general/testNapiRun.js` | `test_general` | C | testNapiRun.js |
| `js-native-api/test_general/testNapiStatus.js` | `test_general` | C | testNapiStatus.js |
| `js-native-api/test_general/testV8Instanceof.js` | `test_general` | C | testV8Instanceof.js |
| `js-native-api/test_general/testV8Instanceof2.js` | `test_general` | C | testV8Instanceof2.js |
| `js-native-api/test_handle_scope/test.js` | `test_handle_scope` | C | test.js |
| `js-native-api/test_new_target/test.js` | `test_new_target` | C | test.js |
| `js-native-api/test_number/test.js` | `test_number` | C | test.js |
| `js-native-api/test_number/test_null.js` | `test_number` | C | test_null.js |
| `js-native-api/test_object/test.js` | `test_object` | C | test.js |
| `js-native-api/test_object/test_exceptions.js` | `test_exceptions` | C | test_exceptions.js |
| `js-native-api/test_object/test_null.js` | `test_object` | C | test_null.js |
| `js-native-api/test_promise/test.js` | `test_promise` | C | test.js |
| `js-native-api/test_properties/test.js` | `test_properties` | C | test.js |
| `js-native-api/test_reference_double_free/test.js` | `test_reference_double_free` | C | test.js |
| `js-native-api/test_reference_double_free/test_wrap.js` | `test_reference_double_free` | C | test_wrap.js |
| `js-native-api/test_symbol/test1.js` | `test_symbol` | C | test1.js |
| `js-native-api/test_symbol/test2.js` | `test_symbol` | C | test2.js |
| `js-native-api/test_symbol/test3.js` | `test_symbol` | C | test3.js |
| `js-native-api/test_typedarray/test.js` | `test_typedarray` | C | test.js |

### Excluded from the runnable subset (24)

| Test (runner id) | Target | Reason (taxonomy is the runner's `reason` field) |
|---|---|---|
| `js-native-api/6_object_wrap/test-object-wrap-ref.js` | `myobject` | driver drives global.gc()/gcUntil; the test host has no gc() hook (N0.5 skeleton scope) -- recorded as needs-gc |
| `js-native-api/7_factory_wrap/test.js` | `7_factory_wrap` | driver drives global.gc()/gcUntil; the test host has no gc() hook (N0.5 skeleton scope) -- recorded as needs-gc |
| `js-native-api/8_passing_wrapped/test.js` | `8_passing_wrapped` | driver drives global.gc()/gcUntil; the test host has no gc() hook (N0.5 skeleton scope) -- recorded as needs-gc |
| `js-native-api/test_function/test.js` | `test_function` | driver drives global.gc()/gcUntil; the test host has no gc() hook (N0.5 skeleton scope) -- recorded as needs-gc |
| `js-native-api/test_general/test.js` | `test_general` | driver drives global.gc()/gcUntil; the test host has no gc() hook (N0.5 skeleton scope) -- recorded as needs-gc |
| `js-native-api/test_general/testFinalizer.js` | `test_general` | driver drives global.gc()/gcUntil; the test host has no gc() hook (N0.5 skeleton scope) -- recorded as needs-gc |
| `js-native-api/test_reference/test.js` | `test_reference` | driver drives global.gc()/gcUntil; the test host has no gc() hook (N0.5 skeleton scope) -- recorded as needs-gc |
| `examples/3-context-awareness/napi/index.js` | `multiple_load` | driver needs Node runtime builtins (child_process / worker_threads / process.* / Buffer) beyond fs-free assert+common |
| `js-native-api/test_exception/testFinalizerException.js` | `test_exception` | driver needs Node runtime builtins (child_process / worker_threads / process.* / Buffer) beyond fs-free assert+common |
| `js-native-api/test_general/testEnvCleanup.js` | `test_general` | driver needs Node runtime builtins (child_process / worker_threads / process.* / Buffer) beyond fs-free assert+common |
| `js-native-api/test_instance_data/test.js` | `test_instance_data` | driver needs Node runtime builtins (child_process / worker_threads / process.* / Buffer) beyond fs-free assert+common |
| `js-native-api/test_reference/test_finalizer.js` | `test_finalizer` | driver needs Node runtime builtins (child_process / worker_threads / process.* / Buffer) beyond fs-free assert+common |
| `js-native-api/6_object_wrap/nested_wrap.js` | `nested_wrap` | binding.gyp defines [NAPI_VERSION=10] require NAPI_VERSION>8 / experimental symbols absent from the v8 provider set |
| `js-native-api/6_object_wrap/test-basic-finalizer.js` | `myobject_basic_finalizer` | binding.gyp defines [NAPI_EXPERIMENTAL] require NAPI_VERSION>8 / experimental symbols absent from the v8 provider set |
| `js-native-api/test_cannot_run_js/test.js` | `test_pending_exception` | binding.gyp defines [NAPI_VERSION=9] require NAPI_VERSION>8 / experimental symbols absent from the v8 provider set |
| `js-native-api/test_finalizer/test.js` | `test_finalizer` | binding.gyp defines [NAPI_EXPERIMENTAL] require NAPI_VERSION>8 / experimental symbols absent from the v8 provider set |
| `js-native-api/test_finalizer/test_fatal_finalize.js` | `test_finalizer` | binding.gyp defines [NAPI_EXPERIMENTAL] require NAPI_VERSION>8 / experimental symbols absent from the v8 provider set |
| `js-native-api/test_sharedarraybuffer/test.js` | `test_sharedarraybuffer` | SharedArrayBuffer API is NAPI_EXPERIMENTAL (node_api_*sharedarraybuffer*), absent at NAPI_VERSION=8 |
| `js-native-api/test_string/test.js` | `test_string` | binding.gyp defines [NAPI_VERSION=10] require NAPI_VERSION>8 / experimental symbols absent from the v8 provider set |
| `js-native-api/test_string/test_null.js` | `test_string` | binding.gyp defines [NAPI_VERSION=10] require NAPI_VERSION>8 / experimental symbols absent from the v8 provider set |
| `examples/5-async-work/async_work_promise/napi/index.js` | `binding` | async-work / TSFN (host surface B4/B5) needs host loop services that the skeleton host does not provide (PR-7) |
| `examples/5-async-work/async_work_thread_safe_function/napi/index.js` | `binding` | async-work / TSFN (host surface B4/B5) needs host loop services that the skeleton host does not provide (PR-7) |
| `examples/6-threadsafe-function/thread_safe_function_round_trip/napi/index.js` | `round_trip` | source includes <uv.h> (libuv); uv headers are never vendored (PR-2) and no host loop is injected (PR-7) |
| `examples/8-tooling/build_with_cmake/napi/hello.js` | `build-napi-with-cmake` | no binding.gyp target mapping (CMake-only tooling example; zig cc/c++ runner does not consume CMakeLists) |

Vendored-but-excluded files stay under `vendor/` for later waves; they
are simply never built or run. Two examples worth noting:

- `js-native-api/test_object/test.js` is runnable; it deliberately does
  an out-of-bounds read (`TypeTaggedInstance(39)`) which upstream
  Release silently tolerates. The runner builds with `-O2`
  (Release-like, node-gyp parity); at `-O0` `zig cc`'s bounds checks
  abort it.
- Async examples (`async_work_promise`,
  `async_work_thread_safe_function`, `thread_safe_function_round_trip`)
  are `provider-gap` even though they would pass under real Node: the
  matrix measures provider capability, and the skeleton host injects no
  loop services (PR-7). Same for `build_with_cmake` (CMake tooling
  example; the runner consumes `binding.gyp`, not `CMakeLists.txt`).

## Not vendored (with reasons)

- Node `test/common/**` (`index.js`, `gc.js`, `require-as.js`, `tick.js`,
  etc.): the upstream test harness; it `require()`s `fs`, `net`,
  `buffer`, `child_process`, `worker_threads`, `process`. Vendoring it
  would put Node builtins in the runnable subset (excluded by the task).
  Replaced by `test/nodeapi/shims/` (`common.js` = buildType/mustCall/
  mustNotCall, `gc.js` = `gcUntil` stub that throws `needs-gc`,
  `assert.js`, `bindings.js`).
- node-addon-examples `**/nan/**` (53 files in 10 dirs at the pin):
  out of scope permanently (plan §0.2: no NAN support).
- node-addon-examples `**/node-addon-api/**` (141 files in 41 dirs):
  C++ API surface in the examples tree; N5.3 instead vendors the
  upstream node-addon-api repository's own test subset
  (`node-addon-api/`, pin below) and runs it through the provider.
  The examples-tree C++ variants remain out of scope.
- node-addon-api repository tests BEYOND the N5.3 subset (see
  `node-addon-api/` pin below): the full upstream suite
  (`test/*.cc/*.js`, ~70 drivers incl. async-worker / TSFN /
  child-process / SharedArrayBuffer tests) was classified from the
  same pin commit but only the selected subset was copied; the rest
  is recorded with skip reasons in `doc/node-api-receipt.md` and
  `test/nodeapi/results/node-addon-api.json` (runner:
  `test/nodeapi/run_node_addon_api_matrix.py`).
- Node `test/node-api/**`: host/runtime suite, not the engine surface;
  out of scope for the js-native-api adoption (N5.1).
- Pinned headers that PR-2 forbids: `<v8.h>`, `<node.h>`,
  `<node_buffer.h>`, `uv.h` were never copied anywhere (not even as
  build inputs). One example (`thread_safe_function_round_trip`)
  includes `<uv.h>` and is therefore `provider-gap`.

## License

Both upstream trees are MIT-licensed; the verbatim `LICENSE` /
`LICENSE.md` copies in this directory are the license texts. The notice
below summarizes the Node.js MIT grant (full text: `js-native-api/LICENSE`;
examples: `examples/LICENSE.md`).

> Node.js is licensed for use as follows:
>
> """
> Copyright Node.js contributors. All rights reserved.
>
> Permission is hereby granted, free of charge, to any person obtaining a
> copy of this software and associated documentation files (the "Software"),
> to deal in the Software without restriction, including without limitation
> the rights to use, copy, modify, merge, publish, distribute, sublicense,
> and/or sell copies of the Software, and to permit persons to whom the
> Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
> FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
> DEALINGS IN THE SOFTWARE.
> """

## Build / run (offline, no network)

Addons are built with `zig cc` / `zig c++` against the vendored headers
(`-INAPI_VERSION=8`, `-DNODE_GYP_MODULE_NAME=<target>`, `-O2`,
`-undefined dynamic_lookup` on macOS; full command in
`run_matrix.py:build_target`). Generated addons and result JSON land in
temp dirs / `test/nodeapi/results/` (git-ignored). Nothing here is built
by node-gyp or npm and nothing is fetched at run time.

```sh
python3 test/nodeapi/run_matrix.py --list
python3 test/nodeapi/run_matrix.py --suite all --host auto
python3 test/nodeapi/run_matrix.py --verify-manifest
```

`--host auto` prefers `tools/nodeapi/ch-nodeapi` when it exists (task
N0.3); otherwise it loads the same `.node` through real Node
(`node-direct`, the N5.1 differential) and records results with
`host: "node-direct"` so a differential pass is never mistaken for a
provider pass.

## Host interop note (input to N5.1)

The N0.3 host binds the addon exports to `globalThis.__exports` and runs
the driver; it installs no CommonJS `require`. The vendored drivers
`require('../../common')`, `require('./build/...')`, `require('assert')`,
and `require('bindings')`, so "through ch-nodeapi" runs at Wave 5 need
the host to resolve those to `test/nodeapi/shims/` (dependency-free for
exactly that). Until then the skeleton runs `node-direct` (full Node
builtins). The runner's half of the contract is frozen: it materializes
`common.js` + `common/gc.js` next to each test tree and sets
`NODE_PATH=<shims dir>` + `NODEAPI_BUILD_DIR=<builddir>` for the driver
process. (Observed during N0.5: `tools/nodeapi/ch-nodeapi.cpp` was
authored against the `--addon`/`--driver` spellings this runner uses;
no host binary had been built in this worktree yet.)
