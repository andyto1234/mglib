; docformat = 'rst'


;= API

pro mg_minmaxscaler::fit, x, y, _extra=e
  compile_opt strictarr

  self->mg_transformer::fit, _extra=e

  dims = size(x, /dimensions)
  n_features = dims[0]

  *self._ranges = make_array(dimension=[n_features, 2], type=size(x, /type))
  x_min = min(x, dimension=2, max=x_max)
  (*self._ranges)[*, 0] = x_min
  (*self._ranges)[*, 1] = x_max
end


function mg_minmaxscaler::transform, x
  compile_opt strictarr

  dims = size(x, /dimensions)
  new_x = make_array(dimension=dims, type=size(x, /type), /nozero)
  n_features = dims[0]

  slopes = 1.0 / ((*self._ranges)[*, 1] - (*self._ranges)[*, 0])
  for f = 0L, n_features - 1L do begin
    new_x[f, *] = slopes[f] * (x[f, *] - (*self._ranges)[f, 0])
  endfor

  return, new_x
end


;= overload methods

function mg_minmaxscaler::_overloadHelp, varname
  compile_opt strictarr

  _type = 'MINMAX'
  _specs = '<>'
  return, string(varname, _type, _specs, format='(%"%-15s %-9s = %s")')
end


;= property access

pro mg_minmaxscaler::getProperty, _ref_extra=e
  compile_opt strictarr

  if (n_elements(e) gt 0L) then self->mg_transformer::getProperty, _extra=e
end


pro mg_minmaxscaler::setProperty, _extra=e
  compile_opt strictarr

  if (n_elements(e) gt 0L) then self->mg_transformer::setProperty, _extra=e
end


;= lifecycle methods

pro mg_minmaxscaler::cleanup
  compile_opt strictarr

  ptr_free, self._ranges
  self->mg_transformer::cleanup
end


function mg_minmaxscaler::init, _extra=e
  compile_opt strictarr

  if (~self->mg_transformer::init(_extra=e)) then return, 0

  self._ranges = ptr_new(/allocate_heap)

  return, 1
end


pro mg_minmaxscaler__define
  compile_opt strictarr

  !null = {mg_minmaxscaler, inherits mg_transformer, $
           _ranges: ptr_new()}
end


; main-level example program

n_features = 5
n_samples = 10
x_train = transpose(findgen(n_samples, n_features))
minmax = mg_minmaxscaler()
new_x_train = minmax->fit_transform(x_train)
x_test = fltarr(n_features, 15)
for f = 0L, n_features - 1L do x_test[f, *] = 10.0 * f + (10.0  - 1.0) * randomu(seed, 15)
new_x_test = minmax->transform(x_test)

print, x_test
print
print, new_x_test

obj_destroy, minmax

end
