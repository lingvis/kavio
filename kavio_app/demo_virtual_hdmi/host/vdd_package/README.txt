VirtualDrivers / Virtual-Display-Driver package
===============================================

This directory contains the VDD files staged with demo_virtual_hdmi. Kavio consumes
the published x64 VirtualDrivers driver package unchanged; INF/DLL/CAT files must
remain byte-for-byte compatible with their public catalog signature.

Required files:
  MttVDD.inf
  MttVDD.dll
  mttvdd.cat
  vdd_settings.xml
  VDD_LICENSE.txt

When the signed driver files are missing, run from the host directory:
  fetch_vdd_package.cmd

The default target is 25.7.23. For development validation of another upstream tag:
  fetch_vdd_package.cmd <tag>

The fetch script downloads a published VirtualDrivers GitHub release, locates the
signed x64 manual driver package, checks the DLL/catalog Authenticode status when
Windows exposes it, copies the driver files into this directory, and records their
SHA-256 values in VDD_SHA256.txt.

Do not edit MttVDD.inf, MttVDD.dll or mttvdd.cat after import. Kavio-specific display
settings are stored in vdd_settings.xml outside the signed driver payload.

Upstream:
  https://github.com/VirtualDrivers/Virtual-Display-Driver
License:
  MIT (see VDD_LICENSE.txt)
