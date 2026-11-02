// H0 (control, NO attach): churn while suspended, resolve later
var out = [];
var r1 = null;
function PR() { return new Promise(function (x) { r1 = x; }); }
async function f(tag) {
    out.push("start" + tag);
    var v1 = await PR();
    out.push("resumed:" + tag + ":" + v1);
}
f(1);
WScript.SetTimeout(function () {
    var sink; for (var g = 0; g < 15; g++) { sink = []; for (var k = 0; k < 12000; k++) sink.push({ q: k }); }
}, 80);
WScript.SetTimeout(function () { r1(7); }, 300);
WScript.SetTimeout(function () { out.forEach(function (s) { WScript.Echo(s); }); WScript.Echo("END"); }, 500);
WScript.Echo("scriptend");
