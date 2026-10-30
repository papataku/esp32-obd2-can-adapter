#!/usr/bin/env python3
"""Build selection and negative-compilation policy checks (no target needed)."""
from pathlib import Path
import configparser
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class PassiveProfile(unittest.TestCase):
    def test_only_receiver_application_is_compiled(self):
        ini=configparser.ConfigParser(interpolation=None)
        ini.read(ROOT/'platformio.ini')
        env=ini['env:m5dial_signal_v2_can_29bit_passive']
        self.assertEqual(env['extends'], 'env:m5dial_legacy_single')
        self.assertEqual(env['build_src_filter'], '-<*> +<passive_rx.cpp>')
        self.assertIn('-DM5CAN_PASSIVE_TX_ISOLATED=1',env['build_flags'])
        self.assertIn('-include include/passive_build_guard.h',env['build_flags'])
        source=(ROOT/'src/passive_rx.cpp').read_text()
        self.assertIn('TWAI_TIMING_CONFIG_500KBITS()',source)
        self.assertIn('TWAI_FILTER_CONFIG_ACCEPT_ALL()',source)
        self.assertNotIn('TWAI_MODE_NORMAL',source)
        self.assertNotIn('twai_transmit',source)
        self.assertNotIn('M5Dial.begin(',source)
        self.assertNotIn('M5.begin(',source)
        self.assertIn('display.init()',source)
        # Guard the exact M5GFX SPI2/default-MISO path that reset TXD.
        self.assertIn('b.spi_host=SPI3_HOST',source)
        self.assertNotIn('b.spi_host=SPI2_HOST',source)
        self.assertIn('b.pin_miso=-1',source)
        self.assertIn('general.tx_queue_len=0',source)
        # No Wire.begin(), IDF I2C initialization or LovyanGFX I2C.
        # StampS3 variant's default SDA=GPIO13 / SCL=GPIO15.
        for forbidden in ('Wire.begin(', 'Wire1.begin(', 'i2cInit(',
                          'i2c_driver_install(', 'i2c_new_master_bus(',
                          'lgfx::i2c::init(', 'TwoWire::begin('):
            self.assertNotIn(forbidden, source)
        self.assertIn('spi_bus_initialize(SPI3_HOST',source)
        self.assertIn('heap_caps_malloc(512',source)
        self.assertIn('display.dmaReady()',source)
    def test_forbidden_sdk_paths_fail_even_in_dead_code(self):
        # The fake SDK declaration verifies poisoning AFTER the SDK include.
        with tempfile.TemporaryDirectory() as d:
            d=Path(d); (d/'driver').mkdir()
            (d/'driver/twai.h').write_text('#pragma once\nvoid twai_transmit();\n')
            for token in ('twai_transmit()', 'TWAI_MODE_NORMAL',
                          'TWAI_MODE_NO_ACK', 'twai_initiate_recovery()'):
                result=subprocess.run(['c++','-x','c++','-fsyntax-only',
                    '-DM5CAN_PASSIVE_TX_ISOLATED=1','-I'+str(d),
                    '-include',str(ROOT/'include/passive_build_guard.h'),'-'],
                    input='void f(){if(false){'+token+';}}',text=True,capture_output=True)
                self.assertNotEqual(result.returncode,0,token)
                self.assertIn('poison',result.stderr)
            result=subprocess.run(['c++','-x','c++','-fsyntax-only',
                '-DM5CAN_PASSIVE_TX_ISOLATED=1','-I'+str(d),
                '-include',str(ROOT/'include/passive_build_guard.h'),'-'],
                input='void f(){}',text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            for flag in ('M5CAN_CAN_TX_ENABLED','M5CAN_VEHICLE_TRIAL',
                         'M5CAN_SIGNAL_V2_CAN_EXPERIMENTAL','M5CAN_CAN_MODE_DRY_RUN',
                         'M5CAN_CAN_TX_HARD_DISABLED'):
                result=subprocess.run(['c++','-x','c++','-fsyntax-only',
                    '-DM5CAN_PASSIVE_TX_ISOLATED=1','-D'+flag+'=1','-I'+str(d),
                    '-include',str(ROOT/'include/passive_build_guard.h'),'-'],
                    input='void f(){}',text=True,capture_output=True)
                self.assertNotEqual(result.returncode,0,flag)
                self.assertIn('Conflicting passive CAN policy',result.stderr)
if __name__=='__main__': unittest.main()
