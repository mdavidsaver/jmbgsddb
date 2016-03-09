
from __future__ import print_function

import unittest
import numpy
from numpy import testing as NT
from numpy.testing import assert_array_almost_equal_nulp as assert_aequal

from .. import Machine

class testBasic(unittest.TestCase):
  def setUp(self):
    self.M = Machine({
      'sim_type':'Vector',
      'elements':[
        {'name':'elem0', 'type':'drift', 'L':1.0e-3},
      ],
    })

  def test_print(self):
    self.assertEqual(str(self.M), """sim_type: Vector
#Elements: 1
Element 0: elem0 (drift)
Transfer: [6,6]((1,1,0,0,0,0),(0,1,0,0,0,0),(0,0,1,1,0,0),(0,0,0,1,0,0),(0,0,0,0,1,0),(0,0,0,0,0,1))
""")

  def test_drift(self):
    "Propogate a state vector through a drift section"

    S = self.M.allocState({})
    self.assertEqual(S.next_elem, 0)

    S.state[:] = [1, 1e-3, 0, 0, 0, 0]
    assert_aequal(S.state, [1.000, 1e-3, 0, 0, 0, 0])

    self.M.propagate(S)

    self.assertEqual(S.next_elem, 1)
    assert_aequal(S.state, [1.001, 1e-3, 0, 0, 0, 0])

    S.next_elem = 2
    self.assertEqual(S.next_elem, 2)

  def test_reconfig(self):
    "Change the length after construction"

    S = self.M.allocState({})

    S.state[:] = [1, 1e-3, 0, 0, 0, 0]
    assert_aequal(S.state, [1.000, 1e-3, 0, 0, 0, 0])

    self.M.propagate(S)

    assert_aequal(S.state, [1.001, 1e-3, 0, 0, 0, 0])

    S.state[:] = [1, 1e-3, 0, 0, 0, 0]
    assert_aequal(S.state, [1.000, 1e-3, 0, 0, 0, 0])

    self.M.reconfigure(0, {"L": 2.0e-3})

    self.M.propagate(S)

    assert_aequal(S.state, [1.002, 1e-3, 0, 0, 0, 0])

    S.state[:] = [1, 1e-3, 0, 0, 0, 0]
    assert_aequal(S.state, [1.000, 1e-3, 0, 0, 0, 0])

    self.M.reconfigure(0, {"L": 5.0e-3})

    self.M.propagate(S)

    assert_aequal(S.state, [1.005, 1e-3, 0, 0, 0, 0])

  def test_gc(self):
    "See that State attributes have appropriate lifetime"

    import weakref, gc

    S = self.M.allocState({})
    state = S.state

    R = weakref.ref(S)

    del S
    gc.collect()

    # S should be kept alive by reference from state
    self.assertIsNot(R(), None)

    del state

    gc.collect()
    S = R()
    self.assertIs(R(), None)

  def test_err(self):
    "Try to propagate the something which is not a State"
    self.assertRaises(ValueError, self.M.propagate, None)

class TestMatrix(unittest.TestCase):
  def setUp(self):
    self.M = Machine({
      'sim_type':'TransferMatrix',
      'elements':[
        {'name':'elem0', 'type':'drift', 'L':1.0e-3},
        {'name':'elem1', 'type':'drift', 'L':1.0e-3},
      ],
    })

  def test_drift(self):
    """Propogate an identity matrix to find
    the cumulative transfer matrix of two drifts
    """

    S = self.M.allocState({})

    assert_aequal(S.state, [
      [1, 0, 0, 0, 0, 0],
      [0, 1, 0, 0, 0, 0],
      [0, 0, 1, 0, 0, 0],
      [0, 0, 0, 1, 0, 0],
      [0, 0, 0, 0, 1, 0],
      [0, 0, 0, 0, 0, 1],
    ])

    self.M.propagate(S)

    assert_aequal(S.state, [
      [1, 2, 0, 0, 0, 0],
      [0, 1, 0, 0, 0, 0],
      [0, 0, 1, 2, 0, 0],
      [0, 0, 0, 1, 0, 0],
      [0, 0, 0, 0, 1, 0],
      [0, 0, 0, 0, 0, 1],
    ])

class TestObserve(unittest.TestCase):
    def setUp(self):
        self.M = Machine(b"""
        sim_type = "Vector";
        L = 2.0e-3;
        elem0: drift, L = 2.0e-3;
        foo: LINE = (elem0*5);
        """)
    def test_all(self):
        S = self.M.allocState({})
        S.state[:] = [0, 0, 1, 1e-3, 0, 0]

        results = self.M.propagate(S, observe=range(5))
        self.assertIsNot(results, None)

        self.assertEqual(results[0][0], 0)
        self.assertEqual(results[1][0], 1)
        self.assertEqual(results[2][0], 2)
        self.assertEqual(results[3][0], 3)
        self.assertEqual(results[4][0], 4)

        assert_aequal(results[0][1].state, [0, 0, 1.002, 1e-3, 0, 0])
        assert_aequal(results[1][1].state, [0, 0, 1.004, 1e-3, 0, 0])
        assert_aequal(results[2][1].state, [0, 0, 1.006, 1e-3, 0, 0])
        assert_aequal(results[3][1].state, [0, 0, 1.008, 1e-3, 0, 0])
        assert_aequal(results[4][1].state, [0, 0, 1.010, 1e-3, 0, 0])

class TestGlobal(unittest.TestCase):
    def test_parse(self):
        "Test global scope when parsing"
        M = Machine(b"""
sim_type = "Vector";
L = 2.0e-3;
elem0: drift;
elem1: drift;
elem2: drift;
foo: LINE = (elem0, elem1, elem2);
""")

        S = M.allocState({})

        S.state[:] = [0, 0, 1, 1e-3, 0, 0]
        assert_aequal(S.state, [0, 0, 1.000, 1e-3, 0, 0])

        M.propagate(S)

        assert_aequal(S.state, [0, 0, 1.006, 1e-3, 0, 0])

class testGeneric(unittest.TestCase):
    def test_generic(self):
        T = numpy.asfarray([
          [1, 0, 1, 0, 1, 0],
          [0, 1, 0, 1, 0, 1],
          [1, 0, 1, 0, 1, 0],
          [0, 1, 0, 1, 0, 1],
          [1, 0, 1, 0, 1, 0],
          [0, 1, 0, 1, 0, 1],
        ])
        M = Machine({
          'sim_type':'TransferMatrix',
          'elements':[
            {'name':'elem0', 'type':'generic', 'transfer':T},
          ],
        })

        S = M.allocState({})

        M.propagate(S)

        assert_aequal(S.state, T)

class TestSource(unittest.TestCase):
    def test_matrix(self):
        T = numpy.asfarray([1, 0, 1, 0, 1, 0])
        M = Machine({
          'sim_type':'Vector',
          'elements':[
            {'name':'elem0', 'type':'source', 'initial':T},
          ],
        })

        S = M.allocState({})

        M.propagate(S)

        assert_aequal(S.state, T)

    def test_matrix(self):
        T = numpy.asfarray([
          [1, 0, 1, 0, 1, 0],
          [0, 1, 0, 1, 0, 1],
          [1, 0, 1, 0, 1, 0],
          [0, 1, 0, 1, 0, 1],
          [1, 0, 1, 0, 1, 0],
          [0, 1, 0, 1, 0, 1],
        ])
        M = Machine({
          'sim_type':'TransferMatrix',
          'elements':[
            {'name':'elem0', 'type':'source', 'initial':T},
          ],
        })

        S = M.allocState({})

        M.propagate(S)

        assert_aequal(S.state, T)

class TestOptimze(unittest.TestCase):
    """Trival example of optimization process

    Adjust a single sector bend to achieve a desired output state
    """
    def setUp(self):
        self.M = Machine(b"""
        sim_type = "Vector";
        straight: drift, L = 1.0e-3;
        bend: sbend, L = 1.0e-1, phi=1.0e-6, K=0;
        foo: LINE = (straight, bend, straight);
        """)


    _expect_K = 3e-3
    _expected = numpy.asfarray([1.10198417, 9.99684702e-04, 1.10201583, 1.00031530e-03, 1.00000000, 1.0e-03])

    def test_expected(self):
        """Test that the expected strength actually results in the expected output state
        """
        S = self.M.allocState({})
        S.state[:] = [1, 1e-3, 1, 1e-3, 1, 1e-3]

        self.M.reconfigure(1, {
            'L':1.0e-1,
            'phi':1.0e-6,
            'K':self._expect_K,
        })

        self.M.propagate(S)

        assert_aequal(S.state, self._expected, 1.0e8)

    def test_optimize(self):
        """Optimize
        """
        p0 = [0.0]

        def resid(p):
            # do each iteration with a clean state (todo: reuse?)
            S = self.M.allocState({})
            S.state[:] = [1, 1e-3, 1, 1e-3, 1, 1e-3] # reset state to initial
            self.M.reconfigure(1, {
                'L':1.0e-1,
                'phi':1.0e-6,
                'K':float(p[0]), # set sbend strength
            })
            self.M.propagate(S)
            D = S.state-self._expected # return difference vector
            print("iterate",p, numpy.square(D).sum())
            return D

        try:
            from scipy.optimize import leastsq
        except ImportError:
            import sys
            print('path')
            print('\n=='.join(sys.path))
            with open('/proc/self/maps') as F:
                print("===========================")
                print(F.read())
            raise

        p1, ier = leastsq(resid, p0)
        print('final',p1,'expect',self._expect_K)

        self.assertIn(ier, range(1,5)) # ier between 1 and 4 is success
        self.assertAlmostEqual(p1[0], self._expect_K, 6)
