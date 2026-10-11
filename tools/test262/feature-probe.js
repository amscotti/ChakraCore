//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Diagnostic observations, not assertions that an edition is conformant.
var observations = [];
function probe(name, action) {
    try {
        observations.push({feature: name, value: String(action())});
    } catch (error) {
        observations.push({feature: name, error: error.name, message: error.message});
    }
}
probe('optional-chaining', function() { return eval('({a: 1})?.a'); });
probe('logical-assignment', function() { return eval('var a = 0; a ||= 1; a'); });
probe('class-fields', function() { return eval('(new (class { x = 1; })()).x'); });
probe('BigInt-default', function() { return typeof BigInt; });
probe('SharedArrayBuffer-default', function() { return typeof SharedArrayBuffer; });
probe('RegExp-symbols-default', function() { return typeof RegExp.prototype[Symbol.match]; });
probe('RegExp-properties-default', function() { return typeof Object.getOwnPropertyDescriptor(RegExp.prototype, 'source'); });
probe('RegExp-named-groups', function() { return new RegExp('(?<name>x)').exec('x').groups.name; });
probe('RegExp-lookbehind', function() { return new RegExp('(?<=a)b').test('ab'); });
probe('RegExp-property-escapes', function() { return new RegExp('\\p{Letter}', 'u').test('a'); });
probe('RegExp-d', function() { return new RegExp('x', 'd').flags; });
probe('RegExp-v', function() { return new RegExp('x', 'v').flags; });
probe('import-attributes', function() { return WScript.LoadModule('import x from "./feature-probe.json" with {type: "json"};', 'samethread'); });
probe('String.matchAll', function() { return typeof String.prototype.matchAll; });
probe('String.replaceAll', function() { return typeof String.prototype.replaceAll; });
probe('Array.flat', function() { return [1, [2]].flat().join(','); });
probe('Array.flatMap', function() { return [1].flatMap(function(v) { return [v, v]; }).join(','); });
probe('error-cause', function() { return new Error('x', {cause: 42}).cause; });
probe('WeakRef', function() { return typeof WeakRef; });
probe('WeakMap-symbols', function() { var map = new WeakMap(); var symbol = Symbol(); map.set(symbol, 1); return map.get(symbol); });
probe('WeakSet-symbols', function() { var symbol = Symbol(); return new WeakSet([symbol]).has(symbol); });
probe('Intl.Locale', function() { return typeof Intl.Locale; });
probe('Array.toSorted', function() { return typeof Array.prototype.toSorted; });
print(JSON.stringify(observations));
print('FEATURE-PROBE-COMPLETE');
