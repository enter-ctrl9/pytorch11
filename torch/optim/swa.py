from collections import defaultdict
from itertools import chain
from torch.optim import Optimizer
import torch
import warnings


class SWA(Optimizer):
    def __init__(self, optimizer, swa_start=None, swa_freq=None, swa_lr=None):
        r"""Implements Stochastic Weight Averaging (SWA).

        Stochastic Weight Averaging was proposed in `Averaging Weights Leads to
        Wider Optima and Better Generalization`_ by Pavel Izmailov, Dmitrii
        Podoprikhin, Timur Garipov, Dmitry Vetrov and Andrew Gordon Wilson
        (UAI 2018).

        SWA is implemented as a wrapper class taking optimizer instance as input
        and applying SWA on top of that optimizer.

        SWA can be used in two modes: automatic and manual. In the automatic
        mode SWA running averages are automatically updated every
        :attr:`swa_freq` steps after :attr:`swa_start` steps of optimization. If
        :attr:`swa_lr` is provided, the learning rate of the optimizer is reset
        to :attr:`swa_lr` at every step starting from :attr:`swa_start`. To use
        SWA in automatic mode provide values for both :attr:`swa_start` and
        :attr:`swa_freq` arguments.

        Alternatively, in the manual mode, use :meth:`update_swa` or
        :meth:`update_swa_group` methods to update the SWA running averages.

        In the end of training use `swap_swa_sgd` method to set the optimized
        variables to the computed averages.

        Args:
            optimizer (torch.optim.Optimizer): optimizer to use with SWA
            swa_start (int): number of steps before starting to apply SWA in
                automatic mode; if None, manual mode is selected (default: None)
            swa_freq (int): number of steps between subsequent updates of
                SWA running averages in automatic mode; if None, manual mode is
                selected (default: None)
            swa_lr (float): learning rate to use starting from step swa_start
                in automatic mode; if None, learning rate is not changed
                (default: None)

        Examples:
            >>> # automatic mode
            >>> base_opt = torch.optim.SGD(model.parameters(), lr=0.1)
            >>> opt = torchcontrib.optim.SWA(
            >>>                 base_opt, swa_start=10, swa_freq=5, swa_lr=0.05)
            >>> for _ in range(100):
            >>>     for input, target in loader:
            >>>         opt.zero_grad()
            >>>         loss_fn(model(input), target).backward()
            >>>         opt.step()
            >>> opt.swap_swa_sgd_update_bn(loader, model)
            >>> # manual mode
            >>> opt = torchcontrib.optim.SWA(base_opt)
            >>> for i in range(100):
            >>>     for input, target in loader:
            >>>         opt.zero_grad()
            >>>         loss_fn(model(input), target).backward()
            >>>         opt.step()
            >>>     if i > 10 and i % 5 == 0:
            >>>         opt.update_swa()
            >>> opt.swap_swa_sgd_update_bn(loader, model)

        .. note::
            Call :meth:`swap_swa_sgd_update_bn` in the end of training to use 
            the computed SWA running averages and update the
            :attr:`running_mean` and :attr:`running_var` statistics of the
            Batch Normalization module.

        .. note::
            If you don't want to update Batch Normalization statistics
            for the SWA solution, you can call :meth:`opt.swap_swa_sgd()` 
            instead of 
            :meth:`opt.swap_swa_sgd_update_bn(loader, model)`.

        .. note::
            If you need to update the statistics of Batch Normalization
            modules, you can do so by using
            `torchcontrib.optim.SWA.bn_update` utility.

        .. note::
            Parameters :attr:`swa_start`, :attr:`swa_freq` represent the number 
            of optimization steps, not epochs.

        .. note::
            See the blogpost
            https://pytorch.org/blog/stochastic-weight-averaging-in-pytorch/
            for an extended description of this SWA implementation.

        .. note::
            The repo https://github.com/izmailovpavel/contrib_swa_examples
            contains examples of using this SWA implementation.

        .. _Averaging Weights Leads to Wider Optima and Better Generalization:
            https://arxiv.org/abs/1803.05407
        .. _There Are Many Consistent Explanations of Unlabeled Data: Why You 
            Should Average:
            https://arxiv.org/abs/1806.05594
        .. _SWALP: Stochastic Weight Averaging in Low-Precision Training:
            https://arxiv.org/abs/1904.11943
        """
        self._auto_mode, (swa_start, swa_freq) = \
            self._check_params(self, swa_start, swa_freq)
        swa_lr = swa_lr
        swa_defaults = dict(
            swa_lr=swa_lr,
            swa_start=swa_start,
            swa_freq=swa_freq)

        if self._auto_mode:
            if swa_start < 0:
                raise ValueError("Invalid swa_start: {}".format(swa_start))
            if swa_freq < 1:
                raise ValueError("Invalid swa_freq: {}".format(swa_freq))
        else:
            if swa_lr is not None:
                warnings.warn(
                    "Some of swa_start, swa_freq is None, ignoring swa_lr")
            # If not in auto mode set all swa parameters to None
            swa_lr = None
            swa_start = None
            swa_freq = None

        if swa_lr is not None and swa_lr < 0:
            raise ValueError("Invalid SWA learning rate: {}".format(swa_lr))

        self._optimizer = optimizer

        self.defaults = self._optimizer.defaults
        self.defaults.update(swa_defaults)
        self.param_groups = self._optimizer.param_groups
        self.state = defaultdict(dict)
        self._opt_state = self._optimizer.state
        for group in self.param_groups:
            group['n_avg'] = 0
            group['step_counter'] = 0
            group['swa_lr'] = swa_lr
            group['swa_start'] = swa_start
            group['swa_freq'] = swa_freq

    @staticmethod
    def _check_params(self, swa_start, swa_freq):
        params = [swa_start, swa_freq]
        params_none = [param is None for param in params]
        if not all(params_none) and any(params_none):
            warnings.warn(
                "Some of swa_start, swa_freq is None, ignoring other")
        for i, param in enumerate(params):
            if param is not None and not isinstance(param, int):
                params[i] = int(param)
                warnings.warn("Casting swa_start, swa_freq to int")
        return not any(params_none), params

    def _reset_lr_to_swa(self):
        for param_group in self.param_groups:
            if param_group['swa_lr'] is None:
                continue
            if param_group['step_counter'] >= param_group['swa_start']:
                param_group['lr'] = param_group['swa_lr']

    def update_swa_group(self, group):
        r"""Updates the SWA running averages for the given parameter group.

        Arguments:
            param_group (dict): Specifies for what parameter group SWA running
                averages should be updated

        Examples:
            >>> # automatic mode
            >>> base_opt = torch.optim.SGD([{'params': [x]},
            >>>             {'params': [y], 'lr': 1e-3}], lr=1e-2, momentum=0.9)
            >>> opt = torchcontrib.optim.SWA(base_opt)
            >>> for i in range(100):
            >>>     opt.zero_grad()
            >>>     loss_fn(model(input), target).backward()
            >>>     opt.step()
            >>>     if i > 10 and i % 5 == 0:
            >>>         # Update SWA for the second parameter group
            >>>         opt.update_swa_group(opt.param_groups[1])
            >>> opt.swap_swa_sgd()
        """
        for p in group['params']:
            param_state = self.state[p]
            if 'swa_buffer' not in param_state:
                param_state['swa_buffer'] = torch.zeros_like(p.data)
            buf = param_state['swa_buffer']
            virtual_decay = 1 / float(group["n_avg"] + 1)
            diff = (p.data - buf) * virtual_decay
            buf.add_(diff)
        group["n_avg"] += 1

    def update_swa(self):
        r"""Updates the SWA running averages of all optimized parameters.
        """
        for group in self.param_groups:
            self.update_swa_group(group)

    def swap_swa_sgd(self):
        r"""Swaps the values of the optimized variables and swa buffers.

        It's meant to be called in the end of training to use the collected
        swa running averages. It can also be used to evaluate the running
        averages during training; to continue training `swap_swa_sgd`
        should be called again.
        """
        for group in self.param_groups:
            for p in group['params']:
                param_state = self.state[p]
                if 'swa_buffer' not in param_state:
                    # If swa wasn't applied we don't swap params
                    warnings.warn(
                        "SWA wasn't applied to param {}; skipping it".format(p))
                    continue
                buf = param_state['swa_buffer']
                tmp = torch.empty_like(p.data)
                tmp.copy_(p.data)
                p.data.copy_(buf)
                buf.copy_(tmp)

    def step(self, closure=None):
        r"""Performs a single optimization step.

        In automatic mode also updates SWA running averages.
        """
        self._reset_lr_to_swa()
        loss = self._optimizer.step(closure)
        for group in self.param_groups:
            group["step_counter"] += 1
            steps = group["step_counter"]
            if self._auto_mode:
                if steps > group['swa_start'] and steps % group['swa_freq'] == 0:
                    self.update_swa_group(group)
        return loss

    def state_dict(self):
        r"""Returns the state of SWA as a :class:`dict`.

        It contains three entries:
            * opt_state - a dict holding current optimization state of the base
                optimizer. Its content differs between optimizer classes.
            * swa_state - a dict containing current state of SWA. For each
                optimized variable it contains swa_buffer keeping the running
                average of the variable
            * param_groups - a dict containing all parameter groups
        """
        opt_state_dict = self._optimizer.state_dict()
        swa_state = {(id(k) if isinstance(k, torch.Tensor) else k): v
                     for k, v in self.state.items()}
        opt_state = opt_state_dict["state"]
        param_groups = opt_state_dict["param_groups"]
        return {"opt_state": opt_state, "swa_state": swa_state,
                "param_groups": param_groups}

    def load_state_dict(self, state_dict):
        r"""Loads the optimizer state.

        Args:
            state_dict (dict): SWA optimizer state. Should be an object returned
                from a call to `state_dict`.
        """
        swa_state_dict = {"state": state_dict["swa_state"],
                          "param_groups": state_dict["param_groups"]}
        opt_state_dict = {"state": state_dict["opt_state"],
                          "param_groups": state_dict["param_groups"]}
        super(SWA, self).load_state_dict(swa_state_dict)
        self._optimizer.load_state_dict(opt_state_dict)
        self._opt_state = self._optimizer.state

    def add_param_group(self, param_group):
        r"""Add a param group to the :class:`Optimizer` s `param_groups`.

        This can be useful when fine tuning a pre-trained network as frozen
        layers can be made trainable and added to the :class:`Optimizer` as
        training progresses.

        Args:
            param_group (dict): Specifies what Tensors should be optimized along
            with group specific optimization options.
        """
        param_group['n_avg'] = 0
        param_group['step_counter'] = 0
        self._optimizer.add_param_group(param_group)

    @staticmethod
    def bn_update(loader, model, device=None):
        r"""Updates BatchNorm running_mean, running_var buffers in the model.

        It performs one pass over data in `loader` to estimate the activation
        statistics for BatchNorm layers in the model.

        Args:
            loader (torch.utils.data.DataLoader): dataset loader to compute the
                activation statistics on. Each data batch should be either a
                tensor, or a list/tuple whose first element is a tensor
                containing data.

            model (torch.nn.Module): model for which we seek to update BatchNorm
                statistics.

            device (torch.device, optional): If set, data will be trasferred to
                :attr:`device` before being passed into :attr:`model`.
        """
        if not _check_bn(model):
            return
        was_training = model.training
        model.train()
        momenta = {}
        model.apply(_reset_bn)
        model.apply(lambda module: _get_momenta(module, momenta))
        n = 0
        for input in loader:
            if isinstance(input, (list, tuple)):
                input = input[0]
            b = input.size(0)

            momentum = b / float(n + b)
            for module in momenta.keys():
                module.momentum = momentum

            if device is not None:
                input = input.to(device)

            model(input)
            n += b

        model.apply(lambda module: _set_momenta(module, momenta))
        model.train(was_training)

    def swap_swa_sgd_update_bn(self, loader, model, device=None):
        r"""Swaps variables and swa buffers and updates BatchNorm buffers.

        This method is equivalent to subsequently calling 
        :meth:`opt.swap_swa_sgd()` and 
        :meth:`opt.bn_update(loader, model, device)`. It's meant to be called in
        the end of training to use the collected swa running averages and update
        the Batch Normalization running activation statistics in the model. It 
        can also be used to evaluate the SWA running averages during training; 
        to continue training `swap_swa_sgd` should be called again.
        """
        self.swap_swa_sgd()
        self.bn_update(loader, model, device)


# BatchNorm utils
def _check_bn_apply(module, flag):
    if issubclass(module.__class__, torch.nn.modules.batchnorm._BatchNorm):
        flag[0] = True


def _check_bn(model):
    flag = [False]
    model.apply(lambda module: _check_bn_apply(module, flag))
    return flag[0]


def _reset_bn(module):
    if issubclass(module.__class__, torch.nn.modules.batchnorm._BatchNorm):
        module.running_mean = torch.zeros_like(module.running_mean)
        module.running_var = torch.ones_like(module.running_var)


def _get_momenta(module, momenta):
    if issubclass(module.__class__, torch.nn.modules.batchnorm._BatchNorm):
        momenta[module] = module.momentum


def _set_momenta(module, momenta):
    if issubclass(module.__class__, torch.nn.modules.batchnorm._BatchNorm):
        module.momentum = momenta[module]
