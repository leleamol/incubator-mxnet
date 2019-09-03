# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

"""Namespace for operators used in Gluon dispatched by F=ndarray."""
from __future__ import absolute_import
from ...context import current_context
from . import _internal as _npi
from ...base import numeric_types


__all__ = ['randint', 'uniform', 'normal']


def randint(low, high=None, size=None, dtype=None, **kwargs):
    """Return random integers from `low` (inclusive) to `high` (exclusive).

    Return random integers from the "discrete uniform" distribution of
    the specified dtype in the "half-open" interval [`low`, `high`). If
    `high` is None (the default), then results are from [0, `low`).

    Parameters
    ----------
    low : int
        Lowest (signed) integer to be drawn from the distribution (unless
        ``high=None``, in which case this parameter is one above the
        *highest* such integer).
    high : int, optional
        If provided, one above the largest (signed) integer to be drawn
        from the distribution (see above for behavior if ``high=None``).
    size : int or tuple of ints, optional
        Output shape.  If the given shape is, e.g., ``(m, n, k)``, then
        ``m * n * k`` samples are drawn.  Default is None, in which case a
        single value is returned.
    dtype : dtype, optional
        Desired dtype of the result. All dtypes are determined by their
        name, i.e., 'int64', 'int', etc, so byteorder is not available
        and a specific precision may have different C types depending
        on the platform. The default value is 'np.int'.
    ctx : Context, optional
        Device context of output. Default is current context.
    out : ndarray, optional
        The output ndarray (default is `None`).

    Returns
    -------
    out : ndarray of ints
        `size`-shaped array of random integers from the appropriate
        distribution, or a single such random int if `size` not provided.

    Examples
    --------
    >>> np.random.randint(2, size=10)
    array([1, 0, 0, 0, 1, 1, 0, 0, 1, 0])
    >>> np.random.randint(1, size=10)
    array([0, 0, 0, 0, 0, 0, 0, 0, 0, 0])

    Generate a 2 x 4 array of ints between 0 and 4, inclusive:

    >>> np.random.randint(5, size=(2, 4))
    array([[4, 0, 2, 1],
        [3, 2, 2, 0]])
    """
    ctx = kwargs.pop('ctx', None)
    out = kwargs.pop('out', None)
    if dtype is None:
        dtype = 'int'
    if ctx is None:
        ctx = current_context()
    if size is None:
        size = 1
    if high is None:
        high = low
        low = 0
    return _npi.random_randint(low, high, shape=size, dtype=dtype, ctx=ctx, out=out)


def uniform(low=0.0, high=1.0, size=None, dtype=None, ctx=None, out=None):
    """Draw samples from a uniform distribution.

    Samples are uniformly distributed over the half-open interval
    ``[low, high)`` (includes low, but excludes high).  In other words,
    any value within the given interval is equally likely to be drawn
    by `uniform`.

    Parameters
    ----------
    low : float, ndarray, optional
        Lower boundary of the output interval.  All values generated will be
        greater than or equal to low.  The default value is 0.
    high : float, ndarray, optional
        Upper boundary of the output interval.  All values generated will be
        less than high.  The default value is 1.0.
    size : int or tuple of ints, optional
        Output shape.  If the given shape is, e.g., ``(m, n, k)``, then
        ``m * n * k`` samples are drawn.  If size is ``None`` (default),
        a scalar tensor containing a single value is returned if
        ``low`` and ``high`` are both scalars.
    dtype : {'float16', 'float32', 'float64'}, optional
        Data type of output samples. Default is 'float32'
    ctx : Context, optional
        Device context of output. Default is current context.

    Returns
    -------
    out : ndarray
        Drawn samples from the parameterized uniform distribution.
    """
    from ...numpy import ndarray as np_ndarray
    input_type = (isinstance(low, np_ndarray), isinstance(high, np_ndarray))
    if dtype is None:
        dtype = 'float32'
    if ctx is None:
        ctx = current_context()
    if out is not None:
        size = out.shape
    if size == ():
        size = None
    if input_type == (True, True):
        return _npi.uniform(low, high, low=None, high=None, size=size,
                            ctx=ctx, dtype=dtype, out=out)
    elif input_type == (False, True):
        return _npi.uniform(high, low=low, high=None, size=size,
                            ctx=ctx, dtype=dtype, out=out)
    elif input_type == (True, False):
        return _npi.uniform(low, low=None, high=high, size=size,
                            ctx=ctx, dtype=dtype, out=out)
    else:
        return _npi.uniform(low=low, high=high, size=size,
                            ctx=ctx, dtype=dtype, out=out)


def normal(loc=0.0, scale=1.0, size=None, **kwargs):
    """Draw random samples from a normal (Gaussian) distribution.

    Samples are distributed according to a normal distribution parametrized
    by *loc* (mean) and *scale* (standard deviation).


    Parameters
    ----------
    loc : float, optional
        Mean (centre) of the distribution.
    scale : float, optional
        Standard deviation (spread or "width") of the distribution.
    size : int or tuple of ints, optional
        Output shape. If the given shape is, e.g., `(m, n, k)`, then `m * n * k`
        samples are drawn. If size is `None` (default), a scalar tensor containing
        a single value is returned if loc and scale are both scalars.
    dtype : {'float16', 'float32', 'float64'}, optional
        Data type of output samples. Default is 'float32'
    ctx : Context, optional
        Device context of output. Default is current context.
    out : ``ndarray``, optional
        Store output to an existing ``ndarray``.

    Returns
    -------
    out : ndarray
        Drawn samples from the parameterized normal distribution.

    Notes
    -----
    This function currently does not support ``loc`` and ``scale`` as ndarrays.
    """
    dtype = kwargs.pop('dtype', None)
    if dtype is None:
        dtype = 'float32'
    ctx = kwargs.pop('ctx', None)
    if ctx is None:
        ctx = current_context()
    out = kwargs.pop('out', None)
    if size is None and out is None:
        size = ()
    if (not isinstance(loc, numeric_types)) or (not isinstance(scale, numeric_types)):
        raise NotImplementedError('np.random.normal only supports loc and scale of '
                                  'numeric types for now')
    return _npi.random_normal(loc, scale, shape=size, dtype=dtype, ctx=ctx, out=out, **kwargs)
