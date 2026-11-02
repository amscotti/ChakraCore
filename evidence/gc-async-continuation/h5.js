// H5: after churn, does a NEW async fn complete while the old suspended one never resumes?
var out = [];
var r1 = null;
function PR() { return new Promise(function (x) { r1 = x; }); }
async function f() {
    out.push("f-start");
    var v = await PR();
    out.push("f-resumed:" + v);
}
async function g() {
    out.push("g-start");
    var v = await Promise.resolve(99);
    out.push("g-resumed:" + v);
}
f();
WScript.SetTimeout(function () {
    var sink; for (var g2 = 0; g2 < 15; g2++) { sink = []; for (var k = 0; k < 12000; k++) sink.push({ q: k }); }
}, 80);
WScript.SetTimeout(function () { r1(7); g(); }, 300);
WScript.SetTimeout(function () { out.forEach(function (s) { WScript.Echo(s); }); WScript.Echo("END"); }, 500);
WScript.Echo("scriptend");
