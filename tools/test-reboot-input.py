#!/usr/bin/env python3
"""Offline reboot gates; never loads drivers, restarts services or records audio."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('reboot',Path(__file__).with_name('validate-reboot-input.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


def state():
    return dict(boot_uuid='11111111-1111-1111-1111-111111111111',kext_uuid='KEXT',
                daemon_running=True,device_visible=True,hashes={str(p):'hash' for p in m.FILES},
                startup=dict(hashes={str(p):'hash' for p in m.STARTUP_FILES}, registered=True,
                             latest_boot_exit=0, load_action="loaded", loaded_uuid="KEXT"),
                kernel=dict(bundle=m.ID,ready=True,state='stopped',session_number=20))


class Tests(unittest.TestCase):
    def setUp(self):
        self.before=state();self.after=copy.deepcopy(self.before)
        self.after['boot_uuid']='22222222-2222-2222-2222-222222222222'
        self.after['kernel'].update(state='prepared',session_number=0)

    def test_pristine_new_boot(self):
        self.assertFalse(m.readiness(self.before))
        self.assertFalse(m.compare(self.before,self.after))

    def test_same_boot_rejected(self):
        self.after['boot_uuid']=self.before['boot_uuid']
        self.assertTrue(m.compare(self.before,self.after))

    def test_startup_receipt_required_when_installed(self):
        self.assertFalse(m.compare(self.before,self.after))
        for field,value in [('registered',False),('latest_boot_exit',None),('latest_boot_exit',1),('load_action','already_loaded'),('load_action',None),('loaded_uuid','OTHER'),('hashes',{'helper':'changed'})]:
            changed=copy.deepcopy(self.after);changed['startup'][field]=value
            self.assertTrue(m.compare(self.before,changed))

    def test_no_loader_cannot_qualify_automatic_start(self):
        self.before.pop('startup')
        self.after.pop('startup')
        self.assertTrue(m.readiness(self.before))
        self.assertTrue(m.compare(self.before,self.after))

    def test_receipt_actions(self):
        log = 'boot_uuid=NEW\nPASS: session ready; recording remains client-controlled\n'
        self.assertEqual(m.receipt_action(log + 'loading qualified session kext', 'NEW'), 'loaded')
        self.assertEqual(m.receipt_action(log + 'load_action=already_loaded', 'NEW'), 'already_loaded')
        self.assertIsNone(m.receipt_action(log, 'NEW'))
        self.assertIsNone(m.receipt_action(log + 'load_action=loaded', 'OLD'))
        self.assertIsNone(m.receipt_action(log + 'load_action=loaded\nexpected session already loaded; no reload', 'NEW'))

    def test_visible_hal_without_kernel_rejected(self):
        self.after.update(kext_uuid=None,kernel={})
        self.assertTrue(m.compare(self.before,self.after))

    def test_prior_recording_active_or_unready_rejected(self):
        for field,value in [('session_number',1),('state','running'),('state',None),('ready',False)]:
            changed=copy.deepcopy(self.after);changed['kernel'][field]=value
            self.assertTrue(m.compare(self.before,changed))

    def test_changed_or_missing_components_rejected(self):
        for field,value in [('kext_uuid','OTHER'),('daemon_running',False),('device_visible',False),('boot_uuid','')]:
            changed=copy.deepcopy(self.after);changed[field]=value
            self.assertTrue(m.compare(self.before,changed))
        for value in ['changed',None]:
            changed=copy.deepcopy(self.after);changed['hashes'][str(m.FILES[0])]=value
            self.assertTrue(m.compare(self.before,changed))


if __name__=='__main__':unittest.main()
