import importlib.util
import unittest
from pathlib import Path
p=Path(__file__).with_name("qualify-gaming-voice.py")
spec=importlib.util.spec_from_file_location("qualifier",p)
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
def example():
    return {"measurement":"exported_frame_time_csv","condition":"gaming_with_gcp_voice",
      "runs_per_condition":3,"average_fps_change_percent":-2,"one_percent_low_fps_change_percent":-3,
      "baseline":{"p99_frame_time_ms":20},"streaming":{"p99_frame_time_ms":21},
      "voice_process":{"median_cpu_percent_total_machine":0.8,"median_peak_private_bytes_sampled":104857600}}
class QualificationTests(unittest.TestCase):
    def test_pass(self):
        self.assertTrue(module.qualify(example(),5,2,150,10)["qualified"])
    def test_fail(self):
        self.assertFalse(module.qualify(example(),1,2,150,10)["qualified"])
    def test_requires_three_runs(self):
        x=example(); x["runs_per_condition"]=2
        with self.assertRaises(ValueError): module.qualify(x,5,2,150,10)
    def test_rejects_bad_limits(self):
        with self.assertRaises(ValueError): module.qualify(example(),float("nan"),2,150,10)
if __name__=="__main__": unittest.main()
