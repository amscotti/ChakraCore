//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------


function write(x) { WScript.Echo(x); }

// var after let at function scope should be redeclaration error
try {
    eval(
            "(function () {" +
            "   let x = 'let x';" +
            "   var x = 'redeclaration error';" +
            "   write(x);" +
            "})();"
        );
} catch (e) {
    write(e);
}


// var after let in a non-function block scope is a redeclaration
// SyntaxError under the Block static-semantics early errors, so the
// block body is never entered.
try {
    eval(
            "(function () {" +
            "   {" +
            "       let x = 'let x';" +
            "       var x = 'var x';" +
            "       write(x);" +
            "   }" +
            "   write(x);" +
            "   {" +
            "       let y = 'let y';" +
            "       write(y);" +
            "       var y = 'var y';" +
            "       write(y);" +
            "   }" +
            "   write(y);" +
            "})();"
        );
} catch (e) {
    write(e);
}

// var before let in a non-function block scope is the same block
// redeclaration SyntaxError.
try {
    eval(
            "(function () {" +
            "   {" +
            "       var x = 'var x';" +
            "       let x = 'let x';" +
            "       write(x);" +
            "   }" +
            "   write(x);" +
            "})();"
        );
} catch (e) {
    write(e);
}
