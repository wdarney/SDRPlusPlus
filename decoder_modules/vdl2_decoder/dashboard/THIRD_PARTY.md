# Interface provenance and licensing

Reference: https://github.com/anthonyborriello/acars-pi-dashboard
Revision inspected: `076b74dd549d2285cb2b988ed25782ed6df895a8`.
Author: Antonio Borriello. Its README explicitly declares MIT, and both Python
file headers identify the author and MIT license. Upstream has no separate
LICENSE file at this revision.

This adaptation retains the live table/filter/expandable-detail approach and
adapts the table and toolbar CSS foundation from `acars_ui.py`. The JSONL reader,
HTTP API, structured tree renderer and SDR++ layout are new. It does not bundle
or launch upstream's `acarsdec` logger. MIT permits incorporation in this GPL-3.0
repository; new work follows the repository license. Upstream attribution is
retained here, in the CSS header, and in the interface footer.

MIT License (for upstream-derived portions)

Copyright (c) Antonio Borriello

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
