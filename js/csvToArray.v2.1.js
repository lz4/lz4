/* Copyright 2012-2013 Daniel Tillin
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * csvToArray v2.1 (Unminifiled for development)
 *
 * For documentation visit:
 * http://code.google.com/p/csv-to-array/
 *
 */
String.prototype.csvToArray = function (o) {
    var od = {
        'fSep': ',',
        'rSep': '\r\n',
        'quot': '"',
        'head': false,
        'trim': false
    }
    if (o) {
        for (var i in od) {
            if (!o[i]) o[i] = od[i];
        }
    } else {
        o = od;
    }
    var a = [
        ['']
    ];
    for (var r = f = p = q = 0; p < this.length; p++) {
        switch (c = this.charAt(p)) {
        case o.quot:
            if (q && this.charAt(p + 1) == o.quot) {
                a[r][f] += o.quot;
                ++p;
            } else {
                q ^= 1;
            }
            break;
        case o.fSep:
            if (!q) {
                if (o.trim) {
                    a[r][f] = a[r][f].replace(/^\s\s*/, '').replace(/\s\s*$/, '');
                }
                a[r][++f] = '';
            } else {
                a[r][f] += c;
            }
            break;
        case o.rSep.charAt(0):
            if (!q && (!o.rSep.charAt(1) || (o.rSep.charAt(1) && o.rSep.charAt(1) == this.charAt(p + 1)))) {
                if (o.trim) {
                    a[r][f] = a[r][f].replace(/^\s\s*/, '').replace(/\s\s*$/, '');
                }
                a[++r] = [''];
                a[r][f = 0] = '';
                if (o.rSep.charAt(1)) {
                    ++p;
                }
            } else {
                a[r][f] += c;
            }
            break;
        default:
            a[r][f] += c;
        }
    }
    if (o.head) {
        a.shift()
    }
    if (a[a.length - 1].length < a[0].length) {
        a.pop()
    }
    return a;
}