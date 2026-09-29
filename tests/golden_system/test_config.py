import importlib.util
from pathlib import Path
import sys
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import generate_golden_config as generator

class CompiledConfig(unittest.TestCase):
    def test_generated_bytes(self):
        self.assertEqual((ROOT/'samples/golden_system/fixed.hpp').read_text(),generator.render())
    def test_domain_budget_allocation_and_admission(self):
        d=generator.contract.read();rates=d['rates'];phases=d['phases']
        counts=[sum(p['rate']==r['id'] for p in phases) for r in rates]
        self.assertEqual(counts,[3,1,1,1,2])
        budgets=[r['budget_ns']//n for r,n in zip(rates,counts)]
        self.assertEqual([b*n for b,n in zip(budgets,counts)],[r['budget_ns'] for r in rates])
        finish=0;records=0
        for t in range(6):
            for r,n,b in zip(rates,counts,budgets):
                if t%r['period_ticks']:continue
                for _ in range(n):
                    finish=max(finish,t*10**7)+b;records+=1
                    self.assertLessEqual(finish,(t+r['period_ticks'])*10**7)
        self.assertEqual(records,27);self.assertLessEqual(finish,6*10**7)
        self.assertGreater(sum((6//r['period_ticks'])*r['budget_ns']*n for r,n in zip(rates,counts)),6*10**7)
    def test_generated_constant_mutation_is_detectable(self):
        generated=generator.render()
        self.assertIn('capacity = 256;',generated)
        self.assertNotEqual(generated.replace('capacity = 256;','capacity = 257;'),generated)

if __name__=='__main__':unittest.main()
