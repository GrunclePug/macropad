# Build configuration, included by the Makefile.

APP    = macropad-config
PREFIX ?= /usr/local
CARGO  ?= cargo
# The size target/ may reach before a build sweeps it, in GiB.
TARGET_MAX ?= 6
WINDOWS = x86_64-pc-windows-gnu
SHOTS  ?= target/screenshots
