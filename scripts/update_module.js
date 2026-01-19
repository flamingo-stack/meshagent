#!/usr/bin/env node
const fs = require('fs');
const zlib = require('zlib');
function updateModule(moduleName) {
    const jsFilePath = `modules/${moduleName}.js`;
    const cFilePath = 'microscript/ILibDuktape_Polyfills.c';
    if (!fs.existsSync(jsFilePath)) {
        console.error(`Error: File ${jsFilePath} not found`);
        process.exit(1);
    }
    console.log(`Updating module: ${moduleName}`);
    // Read and compress JS module
    const jsData = fs.readFileSync(jsFilePath, 'utf8');
    const compressed = zlib.deflateSync(Buffer.from(jsData));
    const base64Data = compressed.toString('base64');
    console.log(`- Module size: ${jsData.length} bytes`);
    console.log(`- Compressed size: ${compressed.length} bytes`);
    console.log(`- Base64 size: ${base64Data.length} bytes`);
    // Get timestamp
    const stat = fs.statSync(jsFilePath);
    const timestamp = new Date(stat.mtime).toISOString().replace(/[:\-]/g, '').replace(/\..+/, '');
    // Read C file
    let cContent = fs.readFileSync(cFilePath, 'utf8');
    // Handle different module formats
    if (moduleName === 'service-manager') {
        // Large module with memcpy_s format
        const varName = '_servicemanager';
        // Find and replace the block
        const blockStartRegex = /char \*_servicemanager = ILibMemory_Allocate\(\d+, 0, NULL, NULL\);/;
        const blockEndRegex = /ILibDuktape_AddCompressedModuleEx\(ctx, "service-manager", _servicemanager, "[^"]+"\);[\s\r\n]+free\(_servicemanager\);/;
        const startMatch = cContent.match(blockStartRegex);
        const endMatch = cContent.match(blockEndRegex);
        if (!startMatch || !endMatch) {
            console.error('Could not find service-manager block in C file');
            process.exit(1);
        }
        const startIdx = cContent.indexOf(startMatch[0]);
        const endIdx = cContent.indexOf(endMatch[0]) + endMatch[0].length;
        // Generate new block
        let newBlock = `char *${varName} = ILibMemory_Allocate(${base64Data.length + 1}, 0, NULL, NULL);`;
        let offset = 0;
        const chunkSize = 16000;
        while (offset < base64Data.length) {
            const chunk = base64Data.substring(offset, offset + chunkSize);
            const remaining = base64Data.length - offset;
            newBlock += `\n\tmemcpy_s(${varName} + ${offset}, ${remaining}, "${chunk}", ${chunk.length});`;
            offset += chunk.length;
        }
        newBlock += `\n\tILibDuktape_AddCompressedModuleEx(ctx, "service-manager", ${varName}, "${timestamp}");`;
        newBlock += `\n\tfree(${varName});`;
        cContent = cContent.substring(0, startIdx) + newBlock + cContent.substring(endIdx);
    } else if (moduleName === 'agent-installer') {
        // agent-installer uses inline duk_peval_string_noresult format
        const inlineRegex = /duk_peval_string_noresult\(ctx, "addCompressedModule\('agent-installer', Buffer\.from\('[^']+', 'base64'\), '[^']+'\);"\);/;
        const inlineMatch = cContent.match(inlineRegex);
        if (inlineMatch) {
            // Inline format found - replace it
            const isoTimestamp = new Date(stat.mtime).toISOString();
            const newInline = `duk_peval_string_noresult(ctx, "addCompressedModule('agent-installer', Buffer.from('${base64Data}', 'base64'), '${isoTimestamp}');");`;
            cContent = cContent.replace(inlineRegex, newInline);
        } else {
            // Try memcpy_s format
            const varName = '_agentinstaller';
            const blockStartRegex = /char \*_agentinstaller = ILibMemory_Allocate\(\d+, 0, NULL, NULL\);/;
            const blockEndRegex = /ILibDuktape_AddCompressedModuleEx\(ctx, "agent-installer", _agentinstaller, "[^"]+"\);/;
            const startMatch = cContent.match(blockStartRegex);
            const endMatch = cContent.match(blockEndRegex);
            if (!startMatch || !endMatch) {
                console.error('Could not find agent-installer block in C file (neither inline nor memcpy_s format)');
                process.exit(1);
            }
            const startIdx = cContent.indexOf(startMatch[0]);
            const endIdx = cContent.indexOf(endMatch[0]) + endMatch[0].length;
            let newBlock = `char *${varName} = ILibMemory_Allocate(${base64Data.length + 1}, 0, NULL, NULL);`;
            let offset = 0;
            const chunkSize = 16000;
            while (offset < base64Data.length) {
                const chunk = base64Data.substring(offset, offset + chunkSize);
                const remaining = base64Data.length - offset;
                newBlock += `\n\tmemcpy_s(${varName} + ${offset}, ${remaining}, "${chunk}", ${chunk.length});`;
                offset += chunk.length;
            }
            newBlock += `\n\tILibDuktape_AddCompressedModuleEx(ctx, "agent-installer", ${varName}, "${timestamp}");`;
            cContent = cContent.substring(0, startIdx) + newBlock + cContent.substring(endIdx);
        }
    } else {
        console.error(`Unknown module: ${moduleName}`);
        process.exit(1);
    }
    // Write updated C file
    fs.writeFileSync(cFilePath, cContent);
    console.log(`Module ${moduleName} updated successfully!`);
}
// Get module name from args
const moduleName = process.argv[2];
if (!moduleName) {
    console.log('Usage: node update_module.js <module-name>');
    console.log('Supported modules: service-manager, agent-installer');
    process.exit(1);
}
updateModule(moduleName);
