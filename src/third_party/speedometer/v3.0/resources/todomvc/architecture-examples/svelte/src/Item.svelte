<script>
    import { createEventDispatcher, tick } from 'svelte';

    export let item;
    export let index;

    let editing = false;
    const dispatch = createEventDispatcher();

    function removeItem() {
        dispatch('removeItem');
    }

    function startEdit() {
       editing = true;
    }

    function handleEdit(event) {
        if (event.key === "Enter")
            event.target.blur();
        else if (event.key === "Escape") 
            editing = false;
    }

    function updateItem(event) {
        if (!editing) return;
        const { value } = event.target;
        if (value.length) {
            item.description = value;
        } else {
          removeItem();
        }
        editing = false;
    }

    async function focusInput(element) {
        await tick();
        element.focus();
    }
</script>

<li class="targeted li-{index}{item.completed ? ' completed' : ''}{editing ? ' editing' : ''}" data-priority={4 - (index % 5)}>
    <div class="targeted view-{index}">
        <input class="toggle" type="checkbox" on:change={(event) => item.completed = event.target.checked} checked={item.completed} />
        <!-- svelte-ignore a11y-label-has-associated-control -->
        <label on:dblclick={startEdit}>{item.description}</label>
        <button on:click={removeItem} class="destroy" />
    </div>

    {#if editing}
        <div class="input-container">
            <input value={item.description} id="edit-todo-input" class="edit" on:keydown={handleEdit} on:blur={updateItem} use:focusInput />
            <label class="visually-hidden" for="edit-todo-input">Edit Todo Input</label>
        </div>
    {/if}
</li>
