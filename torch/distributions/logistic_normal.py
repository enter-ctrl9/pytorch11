import torch
from torch.distributions import constraints
from torch.distributions.normal import Normal
from torch.distributions.transformed_distribution import TransformedDistribution
from torch.distributions.transforms import ComposeTransform, ExpTransform, StickBreakingTransform


class LogisticNormal(TransformedDistribution):
    r"""
    Creates a logistic-normal distribution parameterized by :attr:`loc` and :attr:`scale`
    that define the base `Normal` distribution transformed with the
    `StickBreakingTransform` such that::

        X ~ LogisticNormal(loc, scale)
        Y = log(X / (1 - X.cumsum(-1)))[..., :-1] ~ Normal(loc, scale)

    Args:
        loc (float or Tensor): mean of the base distribution
        scale (float or Tensor): standard deviation of the base distribution

    Example::

        >>> # logistic-normal distributed with mean=(0, 0, 0) and stddev=(1, 1, 1)
        >>> # of the base Normal distribution
        >>> m = distributions.LogisticNormal(torch.tensor([0.0] * 3), torch.tensor([1.0] * 3))
        >>> m.sample()
        tensor([ 0.7653,  0.0341,  0.0579,  0.1427])

    """
    arg_constraints = {'loc': constraints.real, 'scale': constraints.positive}
    support = constraints.simplex
    has_rsample = True

    def __init__(self, loc, scale, validate_args=None):
        base_dist = Normal(loc, scale)
        super(LogisticNormal, self).__init__(base_dist,
                                             StickBreakingTransform(),
                                             validate_args=validate_args)
        # Adjust event shape since StickBreakingTransform adds 1 dimension
        self._event_shape = torch.Size([s + 1 for s in self._event_shape])

    def expand(self, batch_shape, instance=None):
        if not instance and type(self).__init__ is not LogisticNormal.__init__:
            raise NotImplementedError("Subclasses that define a custom __init__ method "
                                      "must also define a custom .expand() method")
        new = self.__new__(type(self)) if not instance else instance
        batch_shape = torch.Size(batch_shape)
        base_dist = self.base_dist.expand(batch_shape + self.base_dist.batch_shape[-1:])
        super(LogisticNormal, new).__init__(base_dist,
                                            StickBreakingTransform(),
                                            validate_args=False)
        new._event_shape = self._event_shape
        new._validate_args = self._validate_args
        return new

    @property
    def loc(self):
        return self.base_dist.loc

    @property
    def scale(self):
        return self.base_dist.scale
