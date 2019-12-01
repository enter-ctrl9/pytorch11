import torch
from .optimizer import Optimizer


class Adam(Optimizer):
    r"""Implements Adam algorithm.

    It has been proposed in `Adam: A Method for Stochastic Optimization`_.

    Arguments:
        params (iterable): iterable of parameters to optimize or dicts defining
            parameter groups
        lr (float, optional): learning rate (default: 1e-3)
        betas (Tuple[float, float], optional): coefficients used for computing
            running averages of gradient and its square (default: (0.9, 0.999))
        eps (float, optional): term added to the denominator to improve
            numerical stability (default: 1e-8)
        weight_decay (float, optional): weight decay (L2 penalty) (default: 0)
        amsgrad (boolean, optional): whether to use the AMSGrad variant of this
            algorithm from the paper `On the Convergence of Adam and Beyond`_
            (default: False)

    .. _Adam\: A Method for Stochastic Optimization:
        https://arxiv.org/abs/1412.6980
    .. _On the Convergence of Adam and Beyond:
        https://openreview.net/forum?id=ryQu7f-RZ
    """

    def __init__(self, params, lr=1e-3, betas=(0.9, 0.999), eps=1e-8,
                 weight_decay=0, amsgrad=False):
        if not 0.0 <= lr:
            raise ValueError("Invalid learning rate: {}".format(lr))
        if not 0.0 <= eps:
            raise ValueError("Invalid epsilon value: {}".format(eps))
        if not 0.0 <= betas[0] < 1.0:
            raise ValueError("Invalid beta parameter at index 0: {}".format(betas[0]))
        if not 0.0 <= betas[1] < 1.0:
            raise ValueError("Invalid beta parameter at index 1: {}".format(betas[1]))
        defaults = dict(lr=lr, betas=betas, eps=eps,
                        weight_decay=weight_decay, amsgrad=amsgrad)
        super(Adam, self).__init__(params, defaults)

    def __setstate__(self, state):
        super(Adam, self).__setstate__(state)
        for group in self.param_groups:
            group.setdefault('amsgrad', False)

    def reset_state(self):
        for group in self.param_groups:
            amsgrad = group['amsgrad']
            for p in group['params']:
                state = self.state[p]
                state['step'] = 0
                # Exponential moving average of gradient values
                state['exp_avg'] = torch.zeros_like(p, memory_format=torch.preserve_format)
                # Exponential moving average of squared gradient values
                state['exp_avg_sq'] = torch.zeros_like(p, memory_format=torch.preserve_format)
                if amsgrad:
                    # Maintains max of all exp. moving avg. of sq. grad. values
                    state['max_exp_avg_sq'] = torch.zeros_like(p, memory_format=torch.preserve_format)

    def get_update(self, p, betas=(.9, .999), eps=1e-8, weight_decay=0, amsgrad=False, **_):
        grad = p.grad
        state = self.state[p]

        if weight_decay > 0:
            grad = grad.add(weight_decay, p)

        beta1, beta2 = betas
        state['step'] += 1
        bias_corr1 = 1 - beta1 ** state['step']
        bias_corr2 = 1 - beta2 ** state['step']
        exp_avg, exp_avg_sq = state['exp_avg'], state['exp_avg_sq']
        exp_avg.mul_(beta1).add_(1 - beta1, grad)
        _var = exp_avg_sq.mul_(beta2).addcmul_(1 - beta2, grad, grad)

        if amsgrad:
            max_exp_avg_sq = state['max_exp_avg_sq']
            _var = torch.max(max_exp_avg_sq, exp_avg_sq, out=max_exp_avg_sq)

        mean = exp_avg / bias_corr1
        var = _var / bias_corr2
        return mean.div_(var.sqrt_().add_(eps))
