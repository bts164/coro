#!/usr/bin/env node
// Runs one on-target test image in an emulated RP2040 (rp2040js), with the
// firmware's UART0 on this process's stdin and stdout. test/target/run_on_target.py
// starts it with --emulator and speaks the same protocol to it as to a board.
//
//   run_rp2040.mjs --bootrom <b1.elf | bootrom.bin> <image.uf2>
//
// The emulator has no boot ROM of its own, and the Pico SDK calls into it (memcpy,
// floating point), so one must be supplied: the ELF from a release of
// https://github.com/raspberrypi/pico-bootrom-rp2040, or the raw 16 KB binary.
//
// Exit status: 3 if the firmware stops (a panic, a failed assert, a hard fault or a
// UBSan trap all end in a breakpoint or an undefined instruction) or does something
// the emulator rejects. Otherwise it runs until
// stdin closes or it is killed.
//
// This is a quick check that the firmware still boots and its tests still pass on
// an ARM core with the board's memory. It is not the board: timing differs, and a
// peripheral is only as faithful as the emulator's model of it.

import { readFileSync } from 'node:fs';
import { Simulator } from 'rp2040js';

const FLASH_START = 0x10000000;
const BOOTROM_SIZE = 16 * 1024;

function fail(message) {
    process.stderr.write(`run_rp2040: ${message}\n`);
    process.exit(2);
}

// The boot ROM as 32-bit words, from an ELF (the loadable segment at address 0) or
// from a raw binary.
function loadBootrom(path) {
    const file = readFileSync(path);
    let rom = file;
    if (file.length >= 52 && file.readUInt32BE(0) === 0x7f454c46) {   // "\x7fELF"
        const phoff = file.readUInt32LE(28);
        const phentsize = file.readUInt16LE(42);
        const phnum = file.readUInt16LE(44);
        rom = null;
        for (let i = 0; i < phnum; i++) {
            const ph = phoff + i * phentsize;
            const type = file.readUInt32LE(ph);
            const offset = file.readUInt32LE(ph + 4);
            const paddr = file.readUInt32LE(ph + 12);
            const filesz = file.readUInt32LE(ph + 16);
            if (type === 1 && paddr === 0 && filesz > 0) {   // PT_LOAD
                rom = file.subarray(offset, offset + filesz);
                break;
            }
        }
        if (rom === null) fail(`${path}: no loadable segment at address 0`);
    }
    if (rom.length > BOOTROM_SIZE) fail(`${path}: larger than the 16 KB boot ROM`);
    const words = new Uint32Array(BOOTROM_SIZE / 4);
    for (let i = 0; i + 4 <= rom.length; i += 4) words[i / 4] = rom.readUInt32LE(i);
    return words;
}

// Copies a UF2 image's blocks into the emulated flash.
function loadUf2(path, flash) {
    const file = readFileSync(path);
    let blocks = 0;
    for (let at = 0; at + 512 <= file.length; at += 512) {
        if (file.readUInt32LE(at) !== 0x0a324655 ||
            file.readUInt32LE(at + 4) !== 0x9e5d5157 ||
            file.readUInt32LE(at + 508) !== 0x0ab16f30) {
            fail(`${path}: not a UF2 file (bad block at offset ${at})`);
        }
        const flags = file.readUInt32LE(at + 8);
        if (flags & 0x1) continue;   // "not main flash": a comment block
        const address = file.readUInt32LE(at + 12);
        const size = file.readUInt32LE(at + 16);
        const offset = address - FLASH_START;
        if (size > 476 || offset < 0 || offset + size > flash.length) {
            fail(`${path}: block at offset ${at} is outside the flash`);
        }
        flash.set(file.subarray(at + 32, at + 32 + size), offset);
        blocks++;
    }
    if (blocks === 0) fail(`${path}: no flash blocks`);
}

// --- arguments --------------------------------------------------------------

let bootromPath = null;
let imagePath = null;
let verbose = false;
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--bootrom') bootromPath = argv[++i];
    else if (argv[i] === '--verbose') verbose = true;
    else if (imagePath === null && !argv[i].startsWith('--')) imagePath = argv[i];
    else fail(`unexpected argument: ${argv[i]}`);
}
if (!bootromPath || !imagePath) {
    fail('usage: run_rp2040.mjs [--verbose] --bootrom <b1.elf | bootrom.bin> <image.uf2>');
}

// --- the machine ------------------------------------------------------------

const simulator = new Simulator();
const mcu = simulator.rp2040;

// The firmware's output, held back until a line is complete: one write per byte is
// slow, and the reader works by lines anyway.
let pendingOutput = [];
function flushOutput() {
    if (pendingOutput.length === 0) return;
    process.stdout.write(Uint8Array.from(pendingOutput));
    pendingOutput = [];
}

// Stops the run. stdout is a pipe, so the last lines are written before exiting.
function stop(message) {
    simulator.stop();
    flushOutput();
    process.stderr.write(`run_rp2040: ${message}\n`);
    process.stdout.write('', () => process.exit(3));
}

// Everything the emulator has to say goes to stderr; stdout carries only the UART.
// The emulator reports an access it cannot model (an unaligned or unmapped
// address) as an error. On the board that is a hard fault, so it ends the run.
mcu.logger = {
    debug() {},
    info() {},
    warn(component, message) {
        if (verbose) process.stderr.write(`run_rp2040: [${component}] ${message}\n`);
    },
    error(component, message) {
        stop(`[${component}] ${message}`);
    },
};

mcu.loadBootrom(loadBootrom(bootromPath));   // also resets the machine and erases the flash
loadUf2(imagePath, mcu.flash);

// The memory protection unit's registers, as plain storage. The emulator does not
// model the MPU and reads an unknown register as all ones; the SDK's stack guard
// (PICO_USE_STACK_GUARDS) takes that to mean the MPU is already in use and stops
// at start-up. With this the firmware boots, but nothing is enforced: a stack
// overflow is caught on the board only.
const MPU_FIRST = 0xd90;   // MPU_TYPE, offset in the 0xe000e000 page
const MPU_LAST = 0xda0;    // MPU_RASR
const mpuRegisters = new Map([[MPU_FIRST, 0x00000800]]);   // MPU_TYPE: 8 regions
const ppb = mcu.ppb;
const ppbRead = ppb.readUint32.bind(ppb);
const ppbWrite = ppb.writeUint32.bind(ppb);
ppb.readUint32 = (offset) =>
    offset >= MPU_FIRST && offset <= MPU_LAST ? (mpuRegisters.get(offset) ?? 0)
                                              : ppbRead(offset);
ppb.writeUint32 = (offset, value) => {
    if (offset > MPU_FIRST && offset <= MPU_LAST) mpuRegisters.set(offset, value >>> 0);
    else ppbWrite(offset, value);
};

// The SDK's panic() and a failed assert end in a breakpoint instruction, and so
// does its hard fault handler. A UBSan trap (with_sanitize=ubsan) is an undefined
// instruction, which the emulator reports the same way.
mcu.onBreak = (code) => stop(`the firmware stopped at a breakpoint or trap (${code}) ` +
                             `near pc 0x${(mcu.core.PC >>> 0).toString(16)}: a panic, a ` +
                             'failed assert or a sanitizer trap');

const uart = mcu.uart[0];
uart.onByte = (value) => {
    pendingOutput.push(value);
    if (value === 0x0a) flushOutput();
};

// Input goes in as fast as the UART's 32-byte receive FIFO has room for it; the
// emulator drops nothing itself, it would overflow the FIFO.
let pendingInput = [];
process.stdin.on('data', (chunk) => {
    for (const byte of chunk) pendingInput.push(byte);
});
process.stdin.on('end', () => process.exit(0));
setInterval(() => {
    // rxFIFO is private in the emulator's TypeScript declarations. Without it,
    // feed a few bytes per turn and rely on the firmware reading them promptly.
    const fifo = uart.rxFIFO;
    let budget = fifo ? Infinity : 8;
    while (pendingInput.length > 0 && budget-- > 0 && !(fifo && fifo.full)) {
        uart.feedByte(pendingInput.shift());
    }
}, 2);

// Start at the second-stage bootloader, as the boot ROM would after reading it
// from flash.
mcu.core.PC = FLASH_START;
simulator.execute();
