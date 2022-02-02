from typing import Iterable, Any, Optional, Callable, Union, List
from .optimizer import Optimizer

class _LRScheduler:
    def __init__(self, optimizer: Optimizer, last_epoch: int=...) -> None: ...
    def state_dict(self) -> dict: ...
    def load_state_dict(self, state_dict: dict) -> None: ...
    def get_last_lr(self) -> List[float]: ...
    def get_lr(self) -> float: ...
    def step(self, epoch: Optional[int]=...) -> None: ...

class LambdaLR(_LRScheduler):
    def __init__(self, optimizer: Optimizer, lr_lambda: Union[Callable[[int], float], List[Callable[[int], float]]], last_epoch: int=...) -> None: ...

class StepLR(_LRScheduler):
    def __init__(self, optimizer: Optimizer, step_size: int, gamma: float=..., last_epoch: int=...) -> None:...

class MultiStepLR(_LRScheduler):
    def __init__(self, optimizer: Optimizer, milestones: Iterable[int], gamma: float=..., last_epoch: int=...) -> None: ...

class ConstantLR(_LRScheduler):
    def __init__(self, optimizer: Optimizer, factor: float=..., total_iters: int=..., last_epoch: int=...) -> None: ...

class LinearLR(_LRScheduler):
    def __init__(self, optimizer: Optimizer, start_factor: float=..., end_factor: float=..., total_iters: int=..., last_epoch: int=...) -> None: ...

class ExponentialLR(_LRScheduler):
    def __init__(self, optimizer: Optimizer, gamma: float, last_epoch: int=...) -> None: ...

class ChainedScheduler(_LRScheduler):
    def __init__(self, schedulers: List[_LRScheduler]) -> None: ...

class SequentialLR(_LRScheduler):
    def __init__(self, optimizer: Optimizer, schedulers: List[_LRScheduler], milestones: List[int], last_epoch: int=..., verbose: bool=...) -> None: ...

class CosineAnnealingLR(_LRScheduler):
    def __init__(self, optimizer: Optimizer, T_max: int, eta_min: float=..., last_epoch: int=...) -> None: ...

class ReduceLROnPlateau:
    in_cooldown: bool

    def __init__(self, optimizer: Optimizer, mode: str=..., factor: float=..., patience: int=..., verbose: bool=..., threshold: float=..., threshold_mode: str=..., cooldown: int=..., min_lr: float=..., eps: float=...) -> None: ...
    def step(self, metrics: Any, epoch: Optional[int]=...) -> None: ...
    def state_dict(self) -> dict: ...
    def load_state_dict(self, state_dict: dict): ...

class CyclicLR(_LRScheduler):
    def __init__(self, optimizer: Optimizer, base_lr: float=..., max_lr: float=..., step_size_up: int=..., step_size_down: int=..., mode: str=..., gamma: float=..., scale_fn: Optional[Callable[[float], float]]=..., scale_mode: str=..., cycle_momentum: bool=..., base_momentum: float=..., max_momentum: float=..., last_epoch: int=...) -> None: ...

class CosineAnnealingWarmRestarts(_LRScheduler):
    def __init__(self, optimizer: Optimizer, T_0: int=..., T_mult: int=..., eta_min: float=..., last_epoch: int=...) -> None: ...
