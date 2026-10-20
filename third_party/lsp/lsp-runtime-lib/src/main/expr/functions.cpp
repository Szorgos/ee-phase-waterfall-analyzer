/*
 * Copyright (C) 2024 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2024 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-runtime-lib
 * Created on: 30 мар. 2024 г.
 *
 * lsp-runtime-lib is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * lsp-runtime-lib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with lsp-runtime-lib. If not, see <https://www.gnu.org/licenses/>.
 */

#include <lsp-plug.in/expr/functions.h>
#include <lsp-plug.in/stdlib/math.h>

namespace lsp
{
    namespace expr
    {
        status_t numeric_compare(value_t *result, const value_t *a, const value_t *b)
        {
            // Check for undef
            if ((a->type == VT_UNDEF) || (b->type == VT_UNDEF))
            {
                set_value_undef(result);
                return STATUS_OK;
            }

            // Check for null
            if (a->type == VT_NULL)
            {
                set_value_bool(result, b->type == VT_NULL);
                return STATUS_OK;
            }
            else if (b->type == VT_NULL)
            {
                set_value_bool(result, false);
                return STATUS_OK;
            }

            // Need to perform forced cast to numeric value?
            value_t tmp_a, tmp_b;
            status_t res;
            init_value(&tmp_a);
            init_value(&tmp_b);
            lsp_finally {
                destroy_value(&tmp_a);
                destroy_value(&tmp_b);
            };

            if (a->type == VT_STRING)
            {
                if ((res = cast_non_string(&tmp_a, a)) != STATUS_OK)
                    return res;
                a = &tmp_a;
            }
            if (b->type == VT_STRING)
            {
                if ((res = cast_non_string(&tmp_b, b)) != STATUS_OK)
                    return res;
                b = &tmp_b;
            }

            // Compare arguments
            ssize_t cmp_result = 0;
            switch (a->type)
            {
                case VT_INT:
                    // Analyze type of b
                    switch (b->type)
                    {
                        case VT_INT:
                            cmp_result =
                                (a->v_int < b->v_int) ? -1 :
                                ((a->v_int > b->v_int) ? 1 : 0);
                            break;
                        case VT_FLOAT:
                            cmp_result =
                                (a->v_int < b->v_float) ? -1 :
                                ((a->v_int > b->v_float) ? 1 : 0);
                            break;
                        case VT_BOOL:
                        {
                            ssize_t value = (b->v_bool) ? 1 : 0;
                            cmp_result =
                                (a->v_int < value) ? -1 :
                                ((a->v_int > value) ? 1 : 0);
                            break;
                        }
                        default:
                            set_value_undef(result);
                            return STATUS_OK;
                    }
                    break;

                case VT_FLOAT:
                    // Analyze type of b
                    switch (b->type)
                    {
                        case VT_INT:
                            cmp_result =
                                (a->v_float < b->v_int) ? -1 :
                                ((a->v_float > b->v_int) ? 1 : 0);
                            break;
                        case VT_FLOAT:
                            cmp_result =
                                (a->v_float < b->v_float) ? -1 :
                                ((a->v_float > b->v_float) ? 1 : 0);
                            break;
                        case VT_BOOL:
                        {
                            double value = (b->v_bool) ? 1.0 : 0.0;
                            cmp_result =
                                (a->v_float < value) ? -1 :
                                ((a->v_float > value) ? 1 : 0);
                            break;
                        }
                        default:
                            set_value_undef(result);
                            return STATUS_OK;
                    }
                    break;

                case VT_BOOL:
                    // Analyze type of b
                    switch (b->type)
                    {
                        case VT_INT:
                        {
                            ssize_t value = (a->v_bool) ? 1 : 0;
                            cmp_result =
                                (value < b->v_int) ? -1 :
                                ((value > b->v_int) ? 1 : 0);
                            break;
                        }
                        case VT_FLOAT:
                        {
                            double value = (a->v_bool) ? 1.0 : 0.0;
                            cmp_result =
                                (value < b->v_float) ? -1 :
                                (value > b->v_float) ? 1 : 0;
                            break;
                        }
                        case VT_BOOL:
                        {
                            cmp_result =
                                (a->v_bool == b->v_bool) ? 0 :
                                (a->v_bool ? 1 : -1);
                            break;
                        }
                        default:
                            set_value_undef(result);
                            return STATUS_OK;
                    }
                    break;

                default:
                    set_value_undef(result);
                    return STATUS_OK;
            }

            set_value_int(result, cmp_result);
            return STATUS_OK;
        }

        status_t stdfunc_min(value_t *result, size_t num_args, const value_t *args)
        {
            // Initialize accumulator
            if (num_args <= 0)
            {
                set_value_undef(result);
                return STATUS_OK;
            }
            status_t res = copy_value(result, &args[0]);
            if (res != STATUS_OK)
                return res;

            // Do the stuff
            value_t cmp;
            init_value(&cmp);
            lsp_finally { destroy_value(&cmp); };

            for (size_t i=1; i<num_args; ++i)
            {
                // Compare values
                if ((res = numeric_compare(&cmp, result, &args[i])) != STATUS_OK)
                {
                    destroy_value(result);
                    return res;
                }
                // Ensure that comparison succeeded
                if (cmp.type != VT_INT)
                {
                    set_value_undef(result);
                    return STATUS_OK;
                }
                // Replace accumulator if the comparison result is greater
                if (cmp.v_int > 0)
                {
                    if ((res = copy_value(result, &args[i])) != STATUS_OK)
                        return res;
                }
            }

            return STATUS_OK;
        }

        status_t stdfunc_max(value_t *result, size_t num_args, const value_t *args)
        {
            // Initialize accumulator
            if (num_args <= 0)
            {
                set_value_undef(result);
                return STATUS_OK;
            }
            status_t res = copy_value(result, &args[0]);
            if (res != STATUS_OK)
                return res;

            // Do the stuff
            value_t cmp;
            init_value(&cmp);
            lsp_finally { destroy_value(&cmp); };

            for (size_t i=1; i<num_args; ++i)
            {
                // Compare values
                if ((res = numeric_compare(&cmp, result, &args[i])) != STATUS_OK)
                {
                    destroy_value(result);
                    return res;
                }
                // Ensure that comparison succeeded
                if (cmp.type != VT_INT)
                {
                    set_value_undef(result);
                    return STATUS_OK;
                }
                // Replace accumulator if the comparison result is greater
                if (cmp.v_int < 0)
                {
                    if ((res = copy_value(result, &args[i])) != STATUS_OK)
                        return res;
                }
            }

            return STATUS_OK;
        }

        status_t stdfunc_avg(value_t *result, size_t num_args, const value_t *args)
        {
            // Initialize accumulator
            if (num_args <= 0)
            {
                set_value_undef(result);
                return STATUS_OK;
            }

            value_t tmp;
            init_value(&tmp);
            lsp_finally { destroy_value(&tmp); };
            set_value_float(result, 0.0);

            for (size_t i=0; i<num_args; ++i)
            {
                status_t res = cast_float(&tmp, &args[i]);
                // Compare values
                if (res != STATUS_OK)
                {
                    destroy_value(result);
                    return res;
                }

                // Ensure that comparison succeeded
                if (tmp.type != VT_FLOAT)
                {
                    set_value_undef(result);
                    return STATUS_OK;
                }

                // Accumulate result
                result->v_float    += tmp.v_float;
            }

            // Compute the average
            result->v_float /= num_args;

            return STATUS_OK;
        }

        status_t stdfunc_rms(value_t *result, size_t num_args, const value_t *args)
        {
            // Initialize accumulator
            if (num_args <= 0)
            {
                set_value_undef(result);
                return STATUS_OK;
            }

            value_t tmp;
            init_value(&tmp);
            lsp_finally { destroy_value(&tmp); };
            set_value_float(result, 0.0);

            for (size_t i=0; i<num_args; ++i)
            {
                status_t res = cast_float(&tmp, &args[i]);
                // Compare values
                if (res != STATUS_OK)
                {
                    destroy_value(result);
                    return res;
                }

                // Ensure that comparison succeeded
                if (tmp.type != VT_FLOAT)
                {
                    set_value_undef(result);
                    return STATUS_OK;
                }

                // Accumulate result
                result->v_float    += tmp.v_float * tmp.v_float;
            }

            // Compute the average
            result->v_float = sqrtf(result->v_float / num_args);

            return STATUS_OK;
        }

    } /* namespace exp */
} /* namespace lsp */

