/* Native evaluation of LHX's own model optimizer records.
 *
 * The supported-image compiler at 0x15f8a constructs vertex addition graphs.
 * Its point emitter at 0x164a8 receives a predecessor vertex and three arrays
 * of signed power-of-two coefficients, BEFORE converting them into x86 code.
 * We retain that original model data and evaluate the same graph directly.
 * No generated machine instructions are inspected or interpreted here.
 *
 * Exact ordering matters: independent rounded matrix powers need not sum to
 * the same value as multiplying the absolute coordinate. Keep the compiler's
 * chosen predecessor, signed coefficients, bit order, and 16/32-bit carries.
 * tools/lift.py enables these observers only for the supported image hash.
 */
#include "model_transform.h"
#include <stdlib.h>
#include <string.h>

enum { MAX_MODELS = 4096, MAX_COMPILER_DEPTH = 32, COEFFICIENT_BITS = 15 };

typedef struct {
    uint16_t vertex, predecessor;
    int8_t coefficient[3][COEFFICIENT_BITS];
} ModelPoint;

typedef struct {
    uint16_t data_segment, lod, output, code_segment, code_offset;
    unsigned count, used;
    int wide, registered;
    uint8_t seen[256];
    ModelPoint *points;
} Model;

static Model *models;
static unsigned model_count, model_capacity;
static unsigned compiler_stack[MAX_COMPILER_DEPTH], compiler_depth;

static void error(CPU *cpu, const char *message)
{
    recomp_unhandled(cpu, cpu->cs, cpu->ip, message);
}

unsigned lhx_model_compile_begin(CPU *cpu)
{
    uint16_t lod = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 14));
    unsigned count = mem_read8(cpu, cpu->ds, (uint16_t)(lod + 1));
    if (compiler_depth == MAX_COMPILER_DEPTH) {
        error(cpu, "model compiler nesting exceeds bound");
        return 0;
    }
    /* Far model records can pass through the same near-memory LOD slot.
     * A reused metadata address does not end the old generated allocation's
     * lifetime; retain its graph until that code address is replaced.
     */
    unsigned index;
    for (index = 0; index < model_count; index++) {
        if (models[index].registered)
            continue;
        unsigned pending;
        for (pending = 0; pending < compiler_depth; pending++)
            if (compiler_stack[pending] == index)
                break;
        if (pending == compiler_depth)
            break;
    }
    if (index == model_count) {
        if (model_count == MAX_MODELS) {
            error(cpu, "model graph registry exceeds bound");
            return 0;
        }
        if (model_count == model_capacity) {
            unsigned capacity = model_capacity ? model_capacity * 2 : 32;
            Model *grown = realloc(models, capacity * sizeof(*grown));
            if (!grown) {
                error(cpu, "cannot allocate native model registry");
                return 0;
            }
            models = grown;
            memset(models + model_capacity, 0,
                   (capacity - model_capacity) * sizeof(*models));
            model_capacity = capacity;
        }
        model_count++;
    }
    Model *model = &models[index];
    free(model->points);
    memset(model, 0, sizeof(*model));
    model->data_segment = cpu->ds;
    model->lod = lod;
    model->output = mem_read16(cpu, cpu->ds, 0x070e);
    model->count = count;
    model->wide = (mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 4)) & 8) != 0;
    if (count && !(model->points = calloc(count, sizeof(*model->points)))) {
        error(cpu, "cannot allocate native model coefficient graph");
        return 0;
    }
    compiler_stack[compiler_depth++] = index;
    return index + 1;
}

void lhx_model_compile_point(CPU *cpu)
{
    if (!compiler_depth) {
        error(cpu, "model point emitted without a compiler context");
        return;
    }
    Model *model = &models[compiler_stack[compiler_depth - 1]];
    uint16_t coefficients = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 2));
    uint16_t previous = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 6));
    uint16_t vertex = mem_read16(cpu, cpu->ss, (uint16_t)(cpu->sp + 8));
    int wide = mem_read8(cpu, cpu->ds, 0xc494) != 0;
    if (cpu->ds != model->data_segment || wide != model->wide ||
        model->used >= model->count || vertex >= model->count || model->seen[vertex] ||
        (previous != 0xff && (previous >= model->count || !model->seen[previous]))) {
        error(cpu, "invalid or unsupported native model dependency graph");
        return;
    }
    ModelPoint *point = &model->points[model->used++];
    point->vertex = vertex;
    point->predecessor = previous;
    for (unsigned axis = 0; axis < 3; axis++)
        for (unsigned bit = 0; bit < COEFFICIENT_BITS; bit++)
            point->coefficient[axis][bit] = (int8_t)mem_read8(
                cpu, cpu->ds, (uint16_t)(coefficients + axis * 16 + bit));
    model->seen[vertex] = 1;
}

void lhx_model_compile_end(CPU *cpu, unsigned token)
{
    if (!token || !compiler_depth || compiler_stack[compiler_depth - 1] != token - 1) {
        error(cpu, "native model compiler context mismatch");
        return;
    }
    compiler_depth--;
    Model *model = &models[token - 1];
    if (!(mem_read8(cpu, model->data_segment, (uint16_t)(model->lod + 13)) & 2))
        return; /* The original compiler declined this model; generic path remains compiled. */
    model->code_offset = mem_read16(cpu, model->data_segment, (uint16_t)(model->lod + 6));
    model->code_segment = mem_read16(cpu, model->data_segment, (uint16_t)(model->lod + 8));
    if (!model->code_segment && !model->code_offset)
        return; /* The authored allocation/emission failure path clears both words. */
    if (model->used != model->count) {
        error(cpu, "compiled model has an incomplete native coefficient graph");
        return;
    }
    for (unsigned i = 0; i < model_count; i++)
        if (models[i].code_segment == model->code_segment &&
            models[i].code_offset == model->code_offset)
            models[i].registered = 0; /* An allocation can be reused by a newly compiled model. */
    model->registered = 1;
}

static void transform(CPU *cpu, const Model *model)
{
    push16(cpu, cpu->si);
    push16(cpu, cpu->di);
    push16(cpu, cpu->cs);
    push16(cpu, 0xffff);
    fn_193C5(cpu); /* Original compiled lookup-table refresh; BX = 12 * scale shift. */

    for (unsigned component = 0; component < 3; component++) {
        cpu->si = (uint16_t)(component * 4);
        cpu->di = (uint16_t)(cpu->bx + component * 4);
        push16(cpu, 0xffff); /* The generated shared axis routine's near-call frame. */
        for (unsigned p = 0; p < model->used; p++) {
            const ModelPoint *point = &model->points[p];
            uint16_t source = point->predecessor == 0xff ? 0x0da4 :
                (uint16_t)(model->output + point->predecessor * 16);
            uint16_t destination = (uint16_t)(model->output + point->vertex * 16 + cpu->si);
            if (model->wide) {
                cpu->ax = mem_read16(cpu, cpu->ds, (uint16_t)(source + cpu->si));
                cpu->dx = mem_read16(cpu, cpu->ds, (uint16_t)(source + cpu->si + 2));
            } else {
                cpu->ax = mem_read16(cpu, cpu->ds, (uint16_t)(source + cpu->si + 2));
            }
            for (unsigned axis = 0; axis < 3; axis++)
                for (unsigned bit = 0; bit < COEFFICIENT_BITS; bit++) {
                    int count = point->coefficient[axis][bit];
                    uint16_t basis = (uint16_t)(0x0874 + (axis * 16 + bit) * 12 + cpu->di);
                    unsigned repetitions = count < 0 ? (unsigned)-count : (unsigned)count;
                    for (unsigned n = 0; n < repetitions; n++) {
                        if (model->wide) {
                            uint16_t low = mem_read16(cpu, cpu->ds, basis);
                            uint16_t high = mem_read16(cpu, cpu->ds, (uint16_t)(basis + 2));
                            if (count > 0) {
                                cpu->ax = flags_add16(cpu, cpu->ax, low);
                                cpu->dx = (uint16_t)flags_adc(cpu, cpu->dx, high, 16);
                            } else {
                                cpu->ax = flags_sub16(cpu, cpu->ax, low);
                                cpu->dx = (uint16_t)flags_sbb(cpu, cpu->dx, high, 16);
                            }
                        } else {
                            uint16_t value = mem_read16(cpu, cpu->ds, (uint16_t)(basis + 2));
                            cpu->ax = count > 0 ? flags_add16(cpu, cpu->ax, value)
                                                : flags_sub16(cpu, cpu->ax, value);
                        }
                    }
                }
            if (model->wide) {
                mem_write16(cpu, cpu->ds, destination, cpu->ax);
                mem_write16(cpu, cpu->ds, (uint16_t)(destination + 2), cpu->dx);
            } else {
                mem_write16(cpu, cpu->ds, (uint16_t)(destination + 2), cpu->ax);
            }
            RECOMP_TICK(cpu);
        }
        cpu->sp = (uint16_t)(cpu->sp + 2);
    }
    cpu->di = pop16(cpu);
    cpu->si = pop16(cpu);
    cpu->sp = (uint16_t)(cpu->sp + 4); /* Generated RETF: host C caller supplies control flow. */
}

int lhx_model_dispatch(CPU *cpu, uint16_t segment, uint16_t offset)
{
    for (unsigned i = 0; i < model_count; i++) {
        const Model *model = &models[i];
        if (!model->registered || model->code_segment != segment || model->code_offset != offset)
            continue;
        if (cpu->ds != model->data_segment) {
            error(cpu, "compiled model called with a different data segment");
            return 1;
        }
        uint16_t old_cs = cpu->cs, old_ip = cpu->ip;
        cpu->cs = segment;
        cpu->ip = offset;
        transform(cpu, model);
        cpu->cs = old_cs;
        cpu->ip = old_ip;
        return 1;
    }
    return 0;
}

int lhx_model_source_vertices(uint16_t code_segment, uint16_t code_offset,
                              int32_t *xyz, unsigned count)
{
    if (!xyz)
        return 0;
    for (unsigned m = 0; m < model_count; m++) {
        const Model *model = models + m;
        if (!model->registered || model->code_segment != code_segment ||
            model->code_offset != code_offset || model->count != count ||
            model->used != count)
            continue;
        for (unsigned p = 0; p < model->used; p++) {
            const ModelPoint *point = model->points + p;
            for (unsigned axis = 0; axis < 3; axis++) {
                int32_t value = point->predecessor == 0xff ? 0 :
                    xyz[point->predecessor * 3 + axis];
                for (unsigned bit = 0; bit < COEFFICIENT_BITS; bit++)
                    value += (int32_t)point->coefficient[axis][bit] * (1 << bit);
                xyz[point->vertex * 3 + axis] = value;
            }
        }
        return 1;
    }
    return 0;
}

void lhx_model_reset(void)
{
    for (unsigned i = 0; i < model_count; i++)
        free(models[i].points);
    free(models);
    models = NULL;
    model_count = model_capacity = compiler_depth = 0;
}
