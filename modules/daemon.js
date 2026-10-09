/*
Copyright 2020 Intel Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/


function stdoutHandler(c)
{
    if (this.parent.parent.options.stdout) { try { process.stdout.write(c); } catch (ex) { } }
}
function exitHandler(code)
{
    if (this.parent.options.crashRestart && code != this.parent.options.exit)
    {
        var now = Date.now();
        if (this.parent.lastRestart != null && (now - this.parent.lastRestart) < 1000)
        {
            this.parent.restartCount = (this.parent.restartCount == null) ? 1 : (this.parent.restartCount + 1);
        }
        else
        {
            this.parent.restartCount = 0;
        }
        this.parent.lastRestart = now;
        if (this.parent.restartCount > 10)
        {
            this.parent.emit('done');
            return;
        }
        var tmp = start(this.parent.path, this.parent.parameters, this.parent.options);
        this.parent.child = tmp.child;
        this.parent.child.parent = this.parent;
    }
    else
    {
        this.parent.emit('done');
    }
}

function start(path, parameters, options)
{
    if (options == null) { options = {}; }
    if (options.exit == null) { options.exit = 0; }
    var ret = { options: options, path: path, parameters: parameters };
    require('events').EventEmitter.call(ret, true)
        .createEvent('done');
    ret.sighandler = function sighandler()
    {
        process.exit();
    };
    ret.sighandler.self = ret;
    if (process.platform != 'win32') { process.on('SIGTERM', ret.sighandler); }
    ret.child = require('child_process').execFile(path, parameters, ret.options);
    ret.child.parent = ret;
    ret.child.stdout.on('data', stdoutHandler);
    ret.child.stderr.on('data', stdoutHandler);
    ret.child.on('exit', exitHandler);
    return (ret);
}

function agent()
{
    var args = process.argv;
    args.splice(1, 1);
    start(process.execPath, args, { crashRestart: true, exit: 6565 }).on('done', function ()
    {
        process.exit();
    });
}

module.exports = { start: start, agent: agent };
